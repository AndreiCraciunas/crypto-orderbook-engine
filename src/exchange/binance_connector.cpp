/**
 * @file binance_connector.cpp
 * @brief Binance WebSocket connector implementation
 */

#include "exchange/binance_connector.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <boost/asio/ssl.hpp>

namespace marketdata {

BinanceConnector::BinanceConnector(bool use_testnet)
    : WebSocketConnector(Exchange::BINANCE)
    , use_testnet_(use_testnet) {

    // Set WebSocket URI
    if (use_testnet) {
        uri_ = "wss://testnet.binance.vision/ws";
    } else {
        uri_ = "wss://stream.binance.com:9443/ws";
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

BinanceConnector::~BinanceConnector() {
    if (is_connected()) {
        disconnect();
    }
}

bool BinanceConnector::connect() {
    if (state_.load() != ConnectionState::DISCONNECTED) {
        spdlog::warn("BinanceConnector: Already connected or connecting");
        return false;
    }

    state_.store(ConnectionState::CONNECTING);
    spdlog::info("BinanceConnector: Connecting to {}", uri_);

    try {
        websocketpp::lib::error_code ec;
        auto con = client_.get_connection(uri_, ec);

        if (ec) {
            spdlog::error("BinanceConnector: Connection creation failed: {}", ec.message());
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
        spdlog::error("BinanceConnector: Exception during connect: {}", e.what());
        state_.store(ConnectionState::FAILED);
        if (error_callback_) {
            error_callback_(std::string("Exception: ") + e.what());
        }
        return false;
    }
}

void BinanceConnector::disconnect() {
    if (state_.load() == ConnectionState::DISCONNECTED) {
        return;
    }

    spdlog::info("BinanceConnector: Disconnecting");
    state_.store(ConnectionState::DISCONNECTED);

    try {
        websocketpp::lib::error_code ec;
        client_.close(connection_, websocketpp::close::status::normal, "Disconnecting", ec);

        if (ec) {
            spdlog::warn("BinanceConnector: Error during disconnect: {}", ec.message());
        }
    } catch (const std::exception& e) {
        spdlog::error("BinanceConnector: Exception during disconnect: {}", e.what());
    }

    running_.store(false);
}

void BinanceConnector::run() {
    running_.store(true);
    spdlog::info("BinanceConnector: Starting I/O loop");

    try {
        client_.run();
    } catch (const std::exception& e) {
        spdlog::error("BinanceConnector: Exception in run loop: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("Run loop exception: ") + e.what());
        }
    }

    spdlog::info("BinanceConnector: I/O loop stopped");
}

void BinanceConnector::stop() {
    spdlog::info("BinanceConnector: Stopping I/O loop");
    running_.store(false);
    client_.stop();
}

bool BinanceConnector::subscribe(const std::string& symbol) {
    if (!is_connected()) {
        spdlog::warn("BinanceConnector: Cannot subscribe - not connected");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        if (subscriptions_.count(symbol)) {
            spdlog::debug("BinanceConnector: Already subscribed to {}", symbol);
            return true;
        }
        subscriptions_.insert(symbol);
    }

    send_subscription(symbol, true);
    spdlog::info("BinanceConnector: Subscribed to {}", symbol);
    return true;
}

bool BinanceConnector::unsubscribe(const std::string& symbol) {
    if (!is_connected()) {
        spdlog::warn("BinanceConnector: Cannot unsubscribe - not connected");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        if (!subscriptions_.count(symbol)) {
            spdlog::debug("BinanceConnector: Not subscribed to {}", symbol);
            return true;
        }
        subscriptions_.erase(symbol);
    }

    send_subscription(symbol, false);
    spdlog::info("BinanceConnector: Unsubscribed from {}", symbol);
    return true;
}

std::string BinanceConnector::get_uri() const {
    return uri_;
}

void BinanceConnector::on_message(ConnectionHdl hdl, MessagePtr msg) {
    try {
        const std::string& payload = msg->get_payload();

        // Parse JSON
        auto json = parser_.parse(payload);

        // Check message type
        if (json["e"].error() == simdjson::SUCCESS) {
            std::string_view event_type = json["e"].get_string().value();

            if (event_type == "depthUpdate") {
                // Parse depth update
                auto update = parse_depth_update(json);
                if (update && orderbook_callback_) {
                    orderbook_callback_(*update);
                }
            }
            // Future: handle trade updates, etc.
        }

    } catch (const simdjson::simdjson_error& e) {
        spdlog::error("BinanceConnector: JSON parse error: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("JSON parse error: ") + e.what());
        }
    } catch (const std::exception& e) {
        spdlog::error("BinanceConnector: Message handling error: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("Message handling error: ") + e.what());
        }
    }
}

void BinanceConnector::on_open(ConnectionHdl hdl) {
    state_.store(ConnectionState::CONNECTED);
    reconnect_count_.store(0);
    spdlog::info("BinanceConnector: Connected to Binance WebSocket");

    if (connection_callback_) {
        connection_callback_(true, "Connected");
    }

    // Resubscribe to all symbols
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    for (const auto& symbol : subscriptions_) {
        send_subscription(symbol, true);
    }
}

void BinanceConnector::on_close(ConnectionHdl hdl) {
    auto prev_state = state_.exchange(ConnectionState::DISCONNECTED);
    spdlog::info("BinanceConnector: Connection closed");

    if (connection_callback_) {
        connection_callback_(false, "Connection closed");
    }

    // Auto-reconnect if enabled
    if (auto_reconnect_ && prev_state == ConnectionState::CONNECTED) {
        uint32_t attempts = reconnect_count_.fetch_add(1);
        if (max_reconnect_attempts_ == 0 || attempts < max_reconnect_attempts_) {
            spdlog::info("BinanceConnector: Attempting reconnect in {}ms (attempt {})",
                        reconnect_delay_ms_, attempts + 1);

            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms_));

            state_.store(ConnectionState::RECONNECTING);
            connect();
        } else {
            spdlog::error("BinanceConnector: Max reconnect attempts reached");
            state_.store(ConnectionState::FAILED);
        }
    }
}

void BinanceConnector::on_fail(ConnectionHdl hdl) {
    state_.store(ConnectionState::FAILED);
    spdlog::error("BinanceConnector: Connection failed");

    if (connection_callback_) {
        connection_callback_(false, "Connection failed");
    }

    // Try to get error reason
    try {
        auto con = client_.get_con_from_hdl(hdl);
        auto ec = con->get_ec();
        spdlog::error("BinanceConnector: Error code: {}", ec.message());

        if (error_callback_) {
            error_callback_("Connection failed: " + ec.message());
        }
    } catch (...) {
        if (error_callback_) {
            error_callback_("Connection failed");
        }
    }
}

BinanceConnector::ContextPtr BinanceConnector::on_tls_init(ConnectionHdl hdl) {
    auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::tlsv12_client);

    try {
        ctx->set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::single_dh_use);

    } catch (const std::exception& e) {
        spdlog::error("BinanceConnector: TLS init error: {}", e.what());
    }

    return ctx;
}

std::optional<NormalizedOrderBookUpdate> BinanceConnector::parse_depth_update(
    const simdjson::dom::element& json) {

    try {
        NormalizedOrderBookUpdate update;
        update.exchange = Exchange::BINANCE;
        update.timestamp = get_timestamp_ns();

        // Get symbol (convert to uppercase with hyphen)
        std::string_view symbol_str = json["s"].get_string().value();
        update.symbol = std::string(symbol_str);  // TODO: normalize format

        // Parse bids
        auto bids_array = json["b"].get_array();
        for (auto bid_element : bids_array) {
            auto bid_array = bid_element.get_array();
            auto it = bid_array.begin();

            std::string_view price_str = (*it).get_string().value();
            ++it;
            std::string_view qty_str = (*it).get_string().value();

            PriceLevelSnapshot level;
            level.price = std::stod(std::string(price_str));
            level.quantity = std::stod(std::string(qty_str));
            level.order_count = 1;  // Binance doesn't provide order count

            update.bids.push_back(level);
        }

        // Parse asks
        auto asks_array = json["a"].get_array();
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

        return update;

    } catch (const std::exception& e) {
        spdlog::error("BinanceConnector: Parse error: {}", e.what());
        return std::nullopt;
    }
}

void BinanceConnector::send_subscription(const std::string& symbol, bool subscribe) {
    try {
        // Build subscription message
        // Binance uses depth@100ms for 100ms snapshots
        std::string stream = symbol + "@depth@100ms";
        std::string method = subscribe ? "SUBSCRIBE" : "UNSUBSCRIBE";

        // Generate ID (simple counter would work)
        static std::atomic<uint64_t> id_counter{1};
        uint64_t id = id_counter.fetch_add(1);

        // Build JSON message
        std::string message = "{"
            "\"method\":\"" + method + "\","
            "\"params\":[\"" + stream + "\"],"
            "\"id\":" + std::to_string(id) +
        "}";

        websocketpp::lib::error_code ec;
        client_.send(connection_, message, websocketpp::frame::opcode::text, ec);

        if (ec) {
            spdlog::error("BinanceConnector: Send subscription error: {}", ec.message());
            if (error_callback_) {
                error_callback_("Send error: " + ec.message());
            }
        } else {
            spdlog::debug("BinanceConnector: Sent {} for {}", method, stream);
        }

    } catch (const std::exception& e) {
        spdlog::error("BinanceConnector: Subscription error: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("Subscription error: ") + e.what());
        }
    }
}

} // namespace marketdata
