/**
 * @file coinbase_connector.cpp
 * @brief Coinbase WebSocket connector implementation
 */

#include "exchange/coinbase_connector.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <boost/asio/ssl.hpp>

namespace marketdata {

CoinbaseConnector::CoinbaseConnector(bool use_sandbox)
    : WebSocketConnector(Exchange::COINBASE)
    , use_sandbox_(use_sandbox) {

    // Set WebSocket URI
    if (use_sandbox) {
        uri_ = "wss://ws-feed-public.sandbox.exchange.coinbase.com";
    } else {
        uri_ = "wss://ws-feed.exchange.coinbase.com";
    }

    // Configure WebSocket client
    client_.clear_access_channels(websocketpp::log::alevel::all);
    client_.clear_error_channels(websocketpp::log::elevel::all);
    client_.set_access_channels(websocketpp::log::alevel::connect);
    client_.set_access_channels(websocketpp::log::alevel::disconnect);
    client_.set_error_channels(websocketpp::log::elevel::warn);
    client_.set_error_channels(websocketpp::log::elevel::rerror);

    // Initialize ASIO
    client_.init_asio();

    // Set handlers
    client_.set_message_handler([this](auto hdl, auto msg) {
        on_message(hdl, msg);
    });

    client_.set_open_handler([this](auto hdl) {
        on_open(hdl);
    });

    client_.set_close_handler([this](auto hdl) {
        on_close(hdl);
    });

    client_.set_fail_handler([this](auto hdl) {
        on_fail(hdl);
    });

    client_.set_tls_init_handler([this](auto hdl) {
        return on_tls_init(hdl);
    });
}

CoinbaseConnector::~CoinbaseConnector() {
    if (is_connected()) {
        disconnect();
    }
}

bool CoinbaseConnector::connect() {
    if (state_.load() != ConnectionState::DISCONNECTED) {
        spdlog::warn("CoinbaseConnector: Already connected or connecting");
        return false;
    }

    state_.store(ConnectionState::CONNECTING);
    spdlog::info("CoinbaseConnector: Connecting to {}", uri_);

    try {
        websocketpp::lib::error_code ec;
        auto con = client_.get_connection(uri_, ec);

        if (ec) {
            spdlog::error("CoinbaseConnector: Connection creation failed: {}", ec.message());
            state_.store(ConnectionState::FAILED);
            if (error_callback_) {
                error_callback_("Connection creation failed: " + ec.message());
            }
            return false;
        }

        connection_ = con->get_handle();
        client_.connect(con);

        return true;

    } catch (const std::exception& e) {
        spdlog::error("CoinbaseConnector: Exception during connect: {}", e.what());
        state_.store(ConnectionState::FAILED);
        if (error_callback_) {
            error_callback_(std::string("Exception: ") + e.what());
        }
        return false;
    }
}

void CoinbaseConnector::disconnect() {
    if (state_.load() == ConnectionState::DISCONNECTED) {
        return;
    }

    spdlog::info("CoinbaseConnector: Disconnecting");
    state_.store(ConnectionState::DISCONNECTED);

    try {
        websocketpp::lib::error_code ec;
        client_.close(connection_, websocketpp::close::status::normal, "Disconnecting", ec);

        if (ec) {
            spdlog::warn("CoinbaseConnector: Error during disconnect: {}", ec.message());
        }
    } catch (const std::exception& e) {
        spdlog::error("CoinbaseConnector: Exception during disconnect: {}", e.what());
    }

    running_.store(false);
}

void CoinbaseConnector::run() {
    running_.store(true);
    spdlog::info("CoinbaseConnector: Starting I/O loop");

    try {
        client_.run();
    } catch (const std::exception& e) {
        spdlog::error("CoinbaseConnector: Exception in run loop: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("Run loop exception: ") + e.what());
        }
    }

    spdlog::info("CoinbaseConnector: I/O loop stopped");
}

void CoinbaseConnector::stop() {
    spdlog::info("CoinbaseConnector: Stopping I/O loop");
    running_.store(false);
    client_.stop();
}

bool CoinbaseConnector::subscribe(const std::string& symbol) {
    if (!is_connected()) {
        spdlog::warn("CoinbaseConnector: Cannot subscribe - not connected");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        if (subscriptions_.count(symbol)) {
            spdlog::debug("CoinbaseConnector: Already subscribed to {}", symbol);
            return true;
        }
        subscriptions_.insert(symbol);
    }

    send_subscription(symbol, true);
    spdlog::info("CoinbaseConnector: Subscribed to {}", symbol);
    return true;
}

bool CoinbaseConnector::unsubscribe(const std::string& symbol) {
    if (!is_connected()) {
        spdlog::warn("CoinbaseConnector: Cannot unsubscribe - not connected");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        if (!subscriptions_.count(symbol)) {
            spdlog::debug("CoinbaseConnector: Not subscribed to {}", symbol);
            return true;
        }
        subscriptions_.erase(symbol);
    }

    send_subscription(symbol, false);
    spdlog::info("CoinbaseConnector: Unsubscribed from {}", symbol);
    return true;
}

std::string CoinbaseConnector::get_uri() const {
    return uri_;
}

void CoinbaseConnector::on_message(ConnectionHdl hdl, MessagePtr msg) {
    try {
        const std::string& payload = msg->get_payload();

        // Parse JSON
        auto json = parser_.parse(payload);

        // Check message type
        if (json["type"].error() == simdjson::SUCCESS) {
            std::string_view msg_type = json["type"].get_string().value();

            if (msg_type == "snapshot") {
                // Parse full snapshot
                auto update = parse_snapshot(json);
                if (update && orderbook_callback_) {
                    orderbook_callback_(*update);
                }
            } else if (msg_type == "l2update") {
                // Parse incremental update
                auto update = parse_l2update(json);
                if (update && orderbook_callback_) {
                    orderbook_callback_(*update);
                }
            } else if (msg_type == "subscriptions") {
                // Subscription confirmation
                spdlog::debug("CoinbaseConnector: Subscription confirmed");
            } else if (msg_type == "error") {
                // Error message
                std::string_view error_msg = json["message"].get_string().value_or("Unknown error");
                spdlog::error("CoinbaseConnector: Server error: {}", error_msg);
                if (error_callback_) {
                    error_callback_(std::string(error_msg));
                }
            }
        }

    } catch (const simdjson::simdjson_error& e) {
        spdlog::error("CoinbaseConnector: JSON parse error: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("JSON parse error: ") + e.what());
        }
    } catch (const std::exception& e) {
        spdlog::error("CoinbaseConnector: Message handling error: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("Message handling error: ") + e.what());
        }
    }
}

void CoinbaseConnector::on_open(ConnectionHdl hdl) {
    state_.store(ConnectionState::CONNECTED);
    reconnect_count_.store(0);
    spdlog::info("CoinbaseConnector: Connected to Coinbase WebSocket");

    if (connection_callback_) {
        connection_callback_(true, "Connected");
    }

    // Resubscribe to all products
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    for (const auto& symbol : subscriptions_) {
        send_subscription(symbol, true);
    }
}

void CoinbaseConnector::on_close(ConnectionHdl hdl) {
    auto prev_state = state_.exchange(ConnectionState::DISCONNECTED);
    spdlog::info("CoinbaseConnector: Connection closed");

    if (connection_callback_) {
        connection_callback_(false, "Connection closed");
    }

    // Auto-reconnect if enabled
    if (auto_reconnect_ && prev_state == ConnectionState::CONNECTED) {
        uint32_t attempts = reconnect_count_.fetch_add(1);
        if (max_reconnect_attempts_ == 0 || attempts < max_reconnect_attempts_) {
            spdlog::info("CoinbaseConnector: Attempting reconnect in {}ms (attempt {})",
                        reconnect_delay_ms_, attempts + 1);

            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms_));

            state_.store(ConnectionState::RECONNECTING);
            connect();
        } else {
            spdlog::error("CoinbaseConnector: Max reconnect attempts reached");
            state_.store(ConnectionState::FAILED);
        }
    }
}

void CoinbaseConnector::on_fail(ConnectionHdl hdl) {
    state_.store(ConnectionState::FAILED);
    spdlog::error("CoinbaseConnector: Connection failed");

    if (connection_callback_) {
        connection_callback_(false, "Connection failed");
    }

    // Try to get error reason
    try {
        auto con = client_.get_con_from_hdl(hdl);
        auto ec = con->get_ec();
        spdlog::error("CoinbaseConnector: Error code: {}", ec.message());

        if (error_callback_) {
            error_callback_("Connection failed: " + ec.message());
        }
    } catch (...) {
        if (error_callback_) {
            error_callback_("Connection failed");
        }
    }
}

CoinbaseConnector::ContextPtr CoinbaseConnector::on_tls_init(ConnectionHdl hdl) {
    auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::tlsv12_client);

    try {
        ctx->set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::single_dh_use);

    } catch (const std::exception& e) {
        spdlog::error("CoinbaseConnector: TLS init error: {}", e.what());
    }

    return ctx;
}

std::optional<NormalizedOrderBookUpdate> CoinbaseConnector::parse_snapshot(
    const simdjson::dom::element& json) {

    try {
        NormalizedOrderBookUpdate update;
        update.exchange = Exchange::COINBASE;
        update.timestamp = get_timestamp_ns();

        // Get product ID
        std::string_view product_id = json["product_id"].get_string().value();
        update.symbol = std::string(product_id);

        // Parse bids array
        auto bids_array = json["bids"].get_array();
        for (auto bid_element : bids_array) {
            auto bid_array = bid_element.get_array();
            auto it = bid_array.begin();

            std::string_view price_str = (*it).get_string().value();
            ++it;
            std::string_view qty_str = (*it).get_string().value();

            PriceLevelSnapshot level;
            level.price = std::stod(std::string(price_str));
            level.quantity = std::stod(std::string(qty_str));
            level.order_count = 1;  // Coinbase doesn't provide order count

            update.bids.push_back(level);
        }

        // Parse asks array
        auto asks_array = json["asks"].get_array();
        for (auto ask_element : asks_array) {
            auto ask_array = ask_element.get_array();
            auto it = ask_array.begin();

            std::string_view price_str = (*it).get_string().value();
            ++it;
            std::string_view qty_str = (*it).get_string().value();

            PriceLevelSnapshot level;
            level.price = std::stod(std::string(price_str));
            level.quantity = std::stod(std::string(qty_str));
            level.order_count = 1;

            update.asks.push_back(level);
        }

        spdlog::debug("CoinbaseConnector: Parsed snapshot for {} ({} bids, {} asks)",
                     update.symbol, update.bids.size(), update.asks.size());

        return update;

    } catch (const std::exception& e) {
        spdlog::error("CoinbaseConnector: Snapshot parse error: {}", e.what());
        return std::nullopt;
    }
}

std::optional<NormalizedOrderBookUpdate> CoinbaseConnector::parse_l2update(
    const simdjson::dom::element& json) {

    try {
        NormalizedOrderBookUpdate update;
        update.exchange = Exchange::COINBASE;
        update.timestamp = get_timestamp_ns();

        // Get product ID
        std::string_view product_id = json["product_id"].get_string().value();
        update.symbol = std::string(product_id);

        // Parse changes array
        // Each change is ["buy"/"sell", "price", "quantity"]
        auto changes_array = json["changes"].get_array();
        for (auto change_element : changes_array) {
            auto change_array = change_element.get_array();
            auto it = change_array.begin();

            std::string_view side_str = (*it).get_string().value();
            ++it;
            std::string_view price_str = (*it).get_string().value();
            ++it;
            std::string_view qty_str = (*it).get_string().value();

            PriceLevelSnapshot level;
            level.price = std::stod(std::string(price_str));
            level.quantity = std::stod(std::string(qty_str));
            level.order_count = 1;

            if (side_str == "buy") {
                update.bids.push_back(level);
            } else if (side_str == "sell") {
                update.asks.push_back(level);
            }
        }

        return update;

    } catch (const std::exception& e) {
        spdlog::error("CoinbaseConnector: L2update parse error: {}", e.what());
        return std::nullopt;
    }
}

void CoinbaseConnector::send_subscription(const std::string& symbol, bool subscribe) {
    try {
        // Build subscription message
        // Coinbase uses "level2_batch" channel for batched L2 updates
        std::string type = subscribe ? "subscribe" : "unsubscribe";

        // Build JSON message
        std::string message = "{"
            "\"type\":\"" + type + "\","
            "\"product_ids\":[\"" + symbol + "\"],"
            "\"channels\":[\"level2_batch\"]"
        "}";

        websocketpp::lib::error_code ec;
        client_.send(connection_, message, websocketpp::frame::opcode::text, ec);

        if (ec) {
            spdlog::error("CoinbaseConnector: Send subscription error: {}", ec.message());
            if (error_callback_) {
                error_callback_("Send error: " + ec.message());
            }
        } else {
            spdlog::debug("CoinbaseConnector: Sent {} for {}", type, symbol);
        }

    } catch (const std::exception& e) {
        spdlog::error("CoinbaseConnector: Subscription error: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("Subscription error: ") + e.what());
        }
    }
}

} // namespace marketdata
