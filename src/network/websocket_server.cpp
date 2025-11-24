/**
 * @file websocket_server.cpp
 * @brief WebSocket server implementation
 */

#include "network/websocket_server.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>

namespace marketdata {

// Helper function to get exchange name
const char* exchange_name(Exchange ex) {
    switch (ex) {
        case Exchange::BINANCE: return "Binance";
        case Exchange::COINBASE: return "Coinbase";
        case Exchange::KRAKEN: return "Kraken";
        default: return "Unknown";
    }
}

WebSocketServer::WebSocketServer(uint16_t port, const std::string& host)
    : port_(port)
    , host_(host) {

    // Configure server
    server_.clear_access_channels(websocketpp::log::alevel::all);
    server_.clear_error_channels(websocketpp::log::elevel::all);
    server_.set_access_channels(websocketpp::log::alevel::connect);
    server_.set_access_channels(websocketpp::log::alevel::disconnect);

    // Initialize Asio
    server_.init_asio();

    // Set handlers
    server_.set_open_handler([this](auto hdl) {
        on_open(hdl);
    });

    server_.set_close_handler([this](auto hdl) {
        on_close(hdl);
    });

    server_.set_message_handler([this](auto hdl, auto msg) {
        on_message(hdl, msg);
    });

    spdlog::info("WebSocketServer: Created on {}:{}", host_, port_);
}

WebSocketServer::~WebSocketServer() {
    if (running_.load()) {
        stop();
    }
}

bool WebSocketServer::start() {
    if (running_.exchange(true)) {
        spdlog::warn("WebSocketServer: Already running");
        return false;
    }

    try {
        // Listen on port
        server_.listen(port_);
        server_.start_accept();

        // Start I/O thread
        io_thread_ = std::make_unique<std::thread>([this]() {
            spdlog::info("WebSocketServer: I/O thread started");
            server_.run();
            spdlog::info("WebSocketServer: I/O thread stopped");
        });

        spdlog::info("WebSocketServer: Listening on {}:{}", host_, port_);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("WebSocketServer: Failed to start: {}", e.what());
        running_.store(false);
        return false;
    }
}

void WebSocketServer::stop() {
    if (!running_.exchange(false)) {
        spdlog::warn("WebSocketServer: Not running");
        return;
    }

    spdlog::info("WebSocketServer: Stopping");

    try {
        // Stop listening
        server_.stop_listening();

        // Close all connections
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (auto& [hdl, sub] : connections_) {
                server_.close(hdl, websocketpp::close::status::going_away, "Server shutdown");
            }
            connections_.clear();
        }

        // Stop server
        server_.stop();

        // Join I/O thread
        if (io_thread_ && io_thread_->joinable()) {
            io_thread_->join();
        }

        spdlog::info("WebSocketServer: Stopped");

    } catch (const std::exception& e) {
        spdlog::error("WebSocketServer: Error during stop: {}", e.what());
    }
}

void WebSocketServer::broadcast_orderbook(const std::string& symbol,
                                         const AggregatedSnapshot& snapshot) {
    std::string message = create_orderbook_message(symbol, snapshot);
    broadcast_to_subscribers(symbol, message);
}

void WebSocketServer::broadcast_analytics(const std::string& symbol,
                                         const MarketAnalytics& analytics) {
    std::string message = create_analytics_message(symbol, analytics);
    broadcast_to_subscribers(symbol, message);
}

void WebSocketServer::broadcast_arbitrage(const ArbitrageOpportunity& opportunity) {
    std::string message = create_arbitrage_message(opportunity);

    // Broadcast to all clients (arbitrage is global)
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto& [hdl, sub] : connections_) {
        send_message(hdl, message);
    }
}

size_t WebSocketServer::get_connection_count() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_.size();
}

void WebSocketServer::on_open(ConnectionHdl hdl) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    ClientSubscription sub;
    connections_[hdl] = sub;

    spdlog::info("WebSocketServer: Client connected (total: {})", connections_.size());
}

void WebSocketServer::on_close(ConnectionHdl hdl) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    connections_.erase(hdl);

    spdlog::info("WebSocketServer: Client disconnected (total: {})", connections_.size());
}

void WebSocketServer::on_message(ConnectionHdl hdl, MessagePtr msg) {
    try {
        const std::string& payload = msg->get_payload();

        // Simple JSON parsing (for production, use proper JSON library)
        // Expected format: {"type":"subscribe","symbol":"BTCUSD","channels":["orderbook"]}

        if (payload.find("\"type\":\"subscribe\"") != std::string::npos) {
            // Extract symbol
            size_t symbol_pos = payload.find("\"symbol\":\"");
            if (symbol_pos != std::string::npos) {
                size_t start = symbol_pos + 10;
                size_t end = payload.find("\"", start);
                std::string symbol = payload.substr(start, end - start);

                // Extract channels
                std::vector<std::string> channels;
                if (payload.find("\"orderbook\"") != std::string::npos) {
                    channels.push_back("orderbook");
                }
                if (payload.find("\"trades\"") != std::string::npos) {
                    channels.push_back("trades");
                }
                if (payload.find("\"analytics\"") != std::string::npos) {
                    channels.push_back("analytics");
                }

                handle_subscribe(hdl, symbol, channels);
            }
        } else if (payload.find("\"type\":\"unsubscribe\"") != std::string::npos) {
            size_t symbol_pos = payload.find("\"symbol\":\"");
            if (symbol_pos != std::string::npos) {
                size_t start = symbol_pos + 10;
                size_t end = payload.find("\"", start);
                std::string symbol = payload.substr(start, end - start);

                handle_unsubscribe(hdl, symbol);
            }
        }

    } catch (const std::exception& e) {
        spdlog::error("WebSocketServer: Message handling error: {}", e.what());
        send_message(hdl, create_error_message(e.what()));
    }
}

void WebSocketServer::handle_subscribe(ConnectionHdl hdl, const std::string& symbol,
                                       const std::vector<std::string>& channels) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(hdl);
    if (it != connections_.end()) {
        it->second.symbols.insert(symbol);

        for (const auto& channel : channels) {
            if (channel == "orderbook") {
                it->second.subscribe_orderbook = true;
            } else if (channel == "trades") {
                it->second.subscribe_trades = true;
            } else if (channel == "analytics") {
                it->second.subscribe_analytics = true;
            }
        }

        spdlog::info("WebSocketServer: Client subscribed to {} (channels: {})",
                    symbol, channels.size());

        // Send confirmation
        std::ostringstream oss;
        oss << "{\"type\":\"subscribed\",\"symbol\":\"" << symbol << "\"}";
        send_message(hdl, oss.str());
    }
}

void WebSocketServer::handle_unsubscribe(ConnectionHdl hdl, const std::string& symbol) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(hdl);
    if (it != connections_.end()) {
        it->second.symbols.erase(symbol);

        spdlog::info("WebSocketServer: Client unsubscribed from {}", symbol);

        // Send confirmation
        std::ostringstream oss;
        oss << "{\"type\":\"unsubscribed\",\"symbol\":\"" << symbol << "\"}";
        send_message(hdl, oss.str());
    }
}

void WebSocketServer::send_message(ConnectionHdl hdl, const std::string& message) {
    try {
        server_.send(hdl, message, websocketpp::frame::opcode::text);
    } catch (const std::exception& e) {
        spdlog::error("WebSocketServer: Send error: {}", e.what());
    }
}

void WebSocketServer::broadcast_to_subscribers(const std::string& symbol,
                                               const std::string& message) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    for (auto& [hdl, sub] : connections_) {
        if (sub.symbols.count(symbol) > 0) {
            send_message(hdl, message);
        }
    }
}

std::string WebSocketServer::create_orderbook_message(const std::string& symbol,
                                                      const AggregatedSnapshot& snapshot) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8);

    oss << "{\"type\":\"orderbook\",\"symbol\":\"" << symbol << "\","
        << "\"timestamp\":" << snapshot.timestamp_ns << ",";

    // Bids
    oss << "\"bids\":[";
    for (size_t i = 0; i < snapshot.bids.size(); i++) {
        if (i > 0) oss << ",";
        const auto& bid = snapshot.bids[i];
        oss << "[" << bid.price << "," << bid.quantity << ",\""
            << exchange_name(bid.exchange) << "\"]";
    }
    oss << "],";

    // Asks
    oss << "\"asks\":[";
    for (size_t i = 0; i < snapshot.asks.size(); i++) {
        if (i > 0) oss << ",";
        const auto& ask = snapshot.asks[i];
        oss << "[" << ask.price << "," << ask.quantity << ",\""
            << exchange_name(ask.exchange) << "\"]";
    }
    oss << "]}";

    return oss.str();
}

std::string WebSocketServer::create_analytics_message(const std::string& symbol,
                                                      const MarketAnalytics& analytics) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    auto spread_stats = analytics.get_spread_stats();

    oss << "{\"type\":\"analytics\",\"symbol\":\"" << symbol << "\","
        << "\"vwap\":" << analytics.get_vwap() << ","
        << "\"twap\":" << analytics.get_twap() << ","
        << "\"imbalance\":" << std::setprecision(4) << analytics.get_imbalance() << ","
        << "\"spread\":" << std::setprecision(2) << spread_stats.current_spread << ","
        << "\"spread_bps\":" << std::setprecision(1) << spread_stats.current_spread_bps
        << "}";

    return oss.str();
}

std::string WebSocketServer::create_arbitrage_message(const ArbitrageOpportunity& opportunity) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    oss << "{\"type\":\"arbitrage\",\"symbol\":\"" << opportunity.symbol << "\","
        << "\"buy_exchange\":\"" << exchange_name(opportunity.buy_exchange) << "\","
        << "\"sell_exchange\":\"" << exchange_name(opportunity.sell_exchange) << "\","
        << "\"buy_price\":" << opportunity.buy_price << ","
        << "\"sell_price\":" << opportunity.sell_price << ","
        << "\"profit_percent\":" << opportunity.profit_percentage << ","
        << "\"max_quantity\":" << opportunity.max_quantity
        << "}";

    return oss.str();
}

std::string WebSocketServer::create_error_message(const std::string& error) {
    std::ostringstream oss;
    oss << "{\"type\":\"error\",\"message\":\"" << error << "\"}";
    return oss.str();
}

} // namespace marketdata
