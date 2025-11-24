/**
 * @file server_main.cpp
 * @brief Main server executable for frontend integration
 *
 * Integrates WebSocket server, REST API server, and exchange connectors
 * to provide real-time market data to the React frontend.
 *
 * Frontend expects:
 * - WebSocket: ws://localhost:8080/stream
 * - REST API: http://localhost:8081/api/v1
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#include "network/message_formatter.hpp"
#include "network/frontend_websocket_server.hpp"
#include "network/frontend_rest_server.hpp"
#include "core/order_book_manager.hpp"
#include "core/order_book.hpp"
#include "metrics/metrics_collector.hpp"
#include "utils/time_utils.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <atomic>
#include <signal.h>

using namespace marketdata;

// Global shutdown flag
std::atomic<bool> shutdown_requested{false};

// Signal handler
void signal_handler(int signal) {
    std::cout << "\n\nShutdown signal received (" << signal << "). Stopping server...\n";
    shutdown_requested.store(true);
}

/**
 * @brief Generate simulated market data
 *
 * Creates realistic order book updates and trades for testing.
 * In production, this would be replaced with real exchange connectors.
 */
class MarketDataSimulator {
public:
    MarketDataSimulator(const std::string& symbol, Exchange exchange)
        : symbol_(symbol)
        , exchange_(exchange)
        , base_price_(50000.0)
        , gen_(std::random_device{}())
        , price_dist_(-50.0, 50.0)
        , qty_dist_(0.1, 5.0)
        , sequence_(0) {
    }

    /**
     * @brief Generate order book update
     */
    NormalizedOrderBookUpdate generate_order_book_update() {
        NormalizedOrderBookUpdate update;
        update.symbol = symbol_;
        update.exchange = exchange_;
        update.timestamp = get_timestamp_ns();
        update.update_id = sequence_;

        // Generate bids (below base price)
        for (int i = 0; i < 10; ++i) {
            double price = base_price_ - (i * 0.5) + (price_dist_(gen_) * 0.1);
            double qty = qty_dist_(gen_);
            uint32_t count = static_cast<uint32_t>(std::abs(price_dist_(gen_)) + 1);
            update.bids.emplace_back(price, qty, count);
        }

        // Generate asks (above base price)
        for (int i = 0; i < 10; ++i) {
            double price = base_price_ + (i * 0.5) + (price_dist_(gen_) * 0.1);
            double qty = qty_dist_(gen_);
            uint32_t count = static_cast<uint32_t>(std::abs(price_dist_(gen_)) + 1);
            update.asks.emplace_back(price, qty, count);
        }

        // Randomly adjust base price (simulate market movement)
        base_price_ += price_dist_(gen_) * 0.01;

        sequence_++;
        return update;
    }

    /**
     * @brief Generate trade
     */
    Trade generate_trade() {
        Trade trade;
        trade.symbol = symbol_;
        trade.exchange = exchange_;
        trade.price = base_price_ + price_dist_(gen_) * 0.5;
        trade.quantity = qty_dist_(gen_) * 0.1;
        trade.timestamp = get_timestamp_ns();
        trade.is_buyer_maker = (price_dist_(gen_) > 0);
        trade.trade_id = std::to_string(get_timestamp_ns());
        return trade;
    }

    uint64_t get_sequence() const { return sequence_; }

private:
    std::string symbol_;
    Exchange exchange_;
    double base_price_;
    std::mt19937 gen_;
    std::uniform_real_distribution<> price_dist_;
    std::uniform_real_distribution<> qty_dist_;
    uint64_t sequence_;
};

// Market data storage for REST API queries
struct MarketDataStore {
    std::mutex mutex;
    std::map<std::string, std::string> latest_orderbook_messages;
    std::map<std::string, std::string> latest_trade_messages;
    std::string latest_metrics_message;
};

int main() {
    try {
        std::cout << "\n";
        std::cout << "================================================================================\n";
        std::cout << "  CRYPTO ORDER BOOK ENGINE - FRONTEND INTEGRATION SERVER\n";
        std::cout << "================================================================================\n\n";

        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // Initialize components
        std::cout << "Initializing components...\n";

        // Create order book manager
        OrderBookManager manager;

        // Create metrics collector
        MetricsCollector metrics;

        // Create market data store for REST API
        MarketDataStore data_store;

        // Create WebSocket server (port 8080 with /stream endpoint)
        FrontendWebSocketServer ws_server(8080);
        if (!ws_server.start()) {
            std::cerr << "Failed to start WebSocket server\n";
            return 1;
        }

        // Create REST API server (port 8081)
        FrontendRestServer rest_server(8081);

        // Set up REST API handlers
        rest_server.set_orderbook_handler([&data_store](const std::string& symbol) -> std::string {
            std::lock_guard<std::mutex> lock(data_store.mutex);
            auto it = data_store.latest_orderbook_messages.find(symbol);
            if (it != data_store.latest_orderbook_messages.end()) {
                return it->second;
            }
            return "{\"error\":\"Symbol not found\"}";
        });

        rest_server.set_symbols_handler([](const std::string&) -> std::string {
            return "{\"symbols\":[\"BTC-USD\",\"ETH-USD\"]}";
        });

        if (!rest_server.start()) {
            std::cerr << "Failed to start REST API server\n";
            ws_server.stop();
            return 1;
        }

        std::cout << "\n";

        // Create market data simulators
        std::cout << "Starting market data simulators...\n";
        MarketDataSimulator btc_binance("BTC-USD", Exchange::BINANCE);
        MarketDataSimulator btc_coinbase("BTC-USD", Exchange::COINBASE);
        MarketDataSimulator eth_binance("ETH-USD", Exchange::BINANCE);

        std::cout << "\n";
        std::cout << "================================================================================\n";
        std::cout << "  SERVER RUNNING\n";
        std::cout << "================================================================================\n";
        std::cout << "\nFrontend Connection:\n";
        std::cout << "  WebSocket: ws://localhost:8080/stream\n";
        std::cout << "  REST API:  http://localhost:8081/api/v1\n";
        std::cout << "\nStreaming Data:\n";
        std::cout << "  - Order book updates (10/sec per exchange)\n";
        std::cout << "  - Trade updates (5/sec per exchange)\n";
        std::cout << "  - Performance metrics (1/sec)\n";
        std::cout << "\nPress Ctrl+C to stop...\n\n";

        // Main server loop
        auto last_order_book_time = std::chrono::steady_clock::now();
        auto last_trade_time = std::chrono::steady_clock::now();
        auto last_metrics_time = std::chrono::steady_clock::now();

        uint64_t message_count = 0;
        uint64_t bytes_sent = 0;

        while (!shutdown_requested.load()) {
            auto now = std::chrono::steady_clock::now();

            // Send order book updates (10 Hz = 100ms interval)
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_order_book_time).count() >= 100) {

                // BTC-USD from Binance
                auto update1 = btc_binance.generate_order_book_update();
                std::string msg1 = MessageFormatter::format_order_book_update(
                    update1, btc_binance.get_sequence());
                ws_server.broadcast(msg1);
                {
                    std::lock_guard<std::mutex> lock(data_store.mutex);
                    data_store.latest_orderbook_messages["BTC-USD"] = msg1;
                }
                message_count++;
                bytes_sent += msg1.length();

                // BTC-USD from Coinbase
                auto update2 = btc_coinbase.generate_order_book_update();
                std::string msg2 = MessageFormatter::format_order_book_update(
                    update2, btc_coinbase.get_sequence());
                ws_server.broadcast(msg2);
                message_count++;
                bytes_sent += msg2.length();

                // ETH-USD from Binance
                auto update3 = eth_binance.generate_order_book_update();
                std::string msg3 = MessageFormatter::format_order_book_update(
                    update3, eth_binance.get_sequence());
                ws_server.broadcast(msg3);
                {
                    std::lock_guard<std::mutex> lock(data_store.mutex);
                    data_store.latest_orderbook_messages["ETH-USD"] = msg3;
                }
                message_count++;
                bytes_sent += msg3.length();

                last_order_book_time = now;
            }

            // Send trade updates (5 Hz = 200ms interval)
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_trade_time).count() >= 200) {

                auto trade = btc_binance.generate_trade();
                std::string msg = MessageFormatter::format_trade(trade);
                ws_server.broadcast(msg);
                message_count++;
                bytes_sent += msg.length();

                last_trade_time = now;
            }

            // Send metrics updates (1 Hz = 1000ms interval)
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_metrics_time).count() >= 1000) {

                // Calculate metrics (simulated values for now)
                uint64_t messages_per_sec = message_count;
                uint64_t bytes_per_sec = bytes_sent;

                std::string msg = MessageFormatter::format_metrics(
                    150,      // p50: 150 ns
                    300,      // p95: 300 ns
                    500,      // p99: 500 ns
                    1200,     // p999: 1200 ns
                    messages_per_sec,
                    bytes_per_sec,
                    "connected",
                    0
                );
                ws_server.broadcast(msg);

                // Print status
                std::cout << "[Status] Messages: " << message_count
                         << " | Throughput: " << messages_per_sec << " msg/s, "
                         << (bytes_per_sec / 1024) << " KB/s\n";

                // Reset counters
                message_count = 0;
                bytes_sent = 0;

                last_metrics_time = now;
            }

            // Sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Cleanup
        std::cout << "\nShutting down...\n";
        ws_server.stop();
        rest_server.stop();

        std::cout << "\nServer stopped successfully.\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}
