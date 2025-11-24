/**
 * @file kraken_connector.cpp
 * @brief Kraken WebSocket connector implementation
 */

#include "exchange/kraken_connector.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <boost/asio/ssl.hpp>

namespace marketdata {

KrakenConnector::KrakenConnector(uint32_t depth)
    : WebSocketConnector(Exchange::KRAKEN)
    , depth_(depth) {

    // Validate depth
    if (depth != 10 && depth != 25 && depth != 100 && depth != 500 && depth != 1000) {
        spdlog::warn("KrakenConnector: Invalid depth {}, using 10", depth);
        depth_ = 10;
    }

    // Set WebSocket URI
    uri_ = "wss://ws.kraken.com";

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

KrakenConnector::~KrakenConnector() {
    if (is_connected()) {
        disconnect();
    }
}

bool KrakenConnector::connect() {
    if (state_.load() != ConnectionState::DISCONNECTED) {
        spdlog::warn("KrakenConnector: Already connected or connecting");
        return false;
    }

    state_.store(ConnectionState::CONNECTING);
    spdlog::info("KrakenConnector: Connecting to {}", uri_);

    try {
        websocketpp::lib::error_code ec;
        auto con = client_.get_connection(uri_, ec);

        if (ec) {
            spdlog::error("KrakenConnector: Connection creation failed: {}", ec.message());
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
        spdlog::error("KrakenConnector: Exception during connect: {}", e.what());
        state_.store(ConnectionState::FAILED);
        if (error_callback_) {
            error_callback_(std::string("Exception: ") + e.what());
        }
        return false;
    }
}

void KrakenConnector::disconnect() {
    if (state_.load() == ConnectionState::DISCONNECTED) {
        return;
    }

    spdlog::info("KrakenConnector: Disconnecting");
    state_.store(ConnectionState::DISCONNECTED);

    try {
        websocketpp::lib::error_code ec;
        client_.close(connection_, websocketpp::close::status::normal, "Disconnecting", ec);

        if (ec) {
            spdlog::warn("KrakenConnector: Error during disconnect: {}", ec.message());
        }
    } catch (const std::exception& e) {
        spdlog::error("KrakenConnector: Exception during disconnect: {}", e.what());
    }

    running_.store(false);
}

void KrakenConnector::run() {
    running_.store(true);
    spdlog::info("KrakenConnector: Starting I/O loop");

    try {
        client_.run();
    } catch (const std::exception& e) {
        spdlog::error("KrakenConnector: Exception in run loop: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("Run loop exception: ") + e.what());
        }
    }

    spdlog::info("KrakenConnector: I/O loop stopped");
}

void KrakenConnector::stop() {
    spdlog::info("KrakenConnector: Stopping I/O loop");
    running_.store(false);
    client_.stop();
}

bool KrakenConnector::subscribe(const std::string& symbol) {
    if (!is_connected()) {
        spdlog::warn("KrakenConnector: Cannot subscribe - not connected");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        if (subscriptions_.count(symbol)) {
            spdlog::debug("KrakenConnector: Already subscribed to {}", symbol);
            return true;
        }
        subscriptions_.insert(symbol);
    }

    send_subscription(symbol, true);
    spdlog::info("KrakenConnector: Subscribed to {} (depth: {})", symbol, depth_);
    return true;
}

bool KrakenConnector::unsubscribe(const std::string& symbol) {
    if (!is_connected()) {
        spdlog::warn("KrakenConnector: Cannot unsubscribe - not connected");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        if (!subscriptions_.count(symbol)) {
            spdlog::debug("KrakenConnector: Not subscribed to {}", symbol);
            return true;
        }
        subscriptions_.erase(symbol);
    }

    send_subscription(symbol, false);
    spdlog::info("KrakenConnector: Unsubscribed from {}", symbol);
    return true;
}

void KrakenConnector::set_depth(uint32_t depth) {
    if (depth != 10 && depth != 25 && depth != 100 && depth != 500 && depth != 1000) {
        spdlog::warn("KrakenConnector: Invalid depth {}, using 10", depth);
        depth_ = 10;
    } else {
        depth_ = depth;
        spdlog::info("KrakenConnector: Depth set to {}", depth_);
    }
}

std::string KrakenConnector::get_uri() const {
    return uri_;
}

void KrakenConnector::on_message(ConnectionHdl hdl, MessagePtr msg) {
    try {
        const std::string& payload = msg->get_payload();

        // Parse JSON
        auto json = parser_.parse(payload);

        // Kraken messages can be objects or arrays
        if (json.is_object()) {
            // Handle event messages (subscription confirmations, errors, etc.)
            if (json["event"].error() == simdjson::SUCCESS) {
                std::string_view event_type = json["event"].get_string().value();

                if (event_type == "systemStatus") {
                    std::string_view status = json["status"].get_string().value();
                    spdlog::info("KrakenConnector: System status: {}", status);
                } else if (event_type == "subscriptionStatus") {
                    std::string_view status = json["status"].get_string().value();
                    spdlog::debug("KrakenConnector: Subscription status: {}", status);

                    // Store channel ID mapping
                    if (status == "subscribed") {
                        int64_t channel_id = json["channelID"].get_int64().value();
                        std::string_view pair = json["pair"].get_string().value();

                        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
                        channel_map_[channel_id] = std::string(pair);
                        spdlog::debug("KrakenConnector: Channel {} -> {}", channel_id, pair);
                    }
                } else if (event_type == "error") {
                    std::string_view error_msg = json["errorMessage"].get_string().value();
                    spdlog::error("KrakenConnector: Server error: {}", error_msg);
                    if (error_callback_) {
                        error_callback_(std::string(error_msg));
                    }
                }
            }
        } else if (json.is_array()) {
            // Handle data messages (snapshots and updates)
            auto array = json.get_array();
            auto it = array.begin();

            // First element is channel ID
            int64_t channel_id = (*it).get_int64().value();
            ++it;

            // Get pair from channel map
            std::string pair;
            {
                std::lock_guard<std::mutex> lock(subscriptions_mutex_);
                auto map_it = channel_map_.find(channel_id);
                if (map_it != channel_map_.end()) {
                    pair = map_it->second;
                } else {
                    // Try to get from last element (some messages include it)
                    auto last_it = array.end();
                    --last_it;
                    if ((*last_it).is_string()) {
                        std::string_view pair_str = (*last_it).get_string().value();
                        pair = std::string(pair_str);
                    }
                }
            }

            if (pair.empty()) {
                spdlog::debug("KrakenConnector: Unknown channel ID {}", channel_id);
                return;
            }

            // Second element is the data object
            auto data = *it;

            // Check if this is a snapshot (has "as" and "bs") or update (has "a" and "b")
            bool is_snapshot = (data["as"].error() == simdjson::SUCCESS ||
                               data["bs"].error() == simdjson::SUCCESS);

            std::optional<NormalizedOrderBookUpdate> update;
            if (is_snapshot) {
                update = parse_snapshot(data, pair);
            } else {
                update = parse_update(data, pair);
            }

            if (update && orderbook_callback_) {
                orderbook_callback_(*update);
            }
        }

    } catch (const simdjson::simdjson_error& e) {
        spdlog::error("KrakenConnector: JSON parse error: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("JSON parse error: ") + e.what());
        }
    } catch (const std::exception& e) {
        spdlog::error("KrakenConnector: Message handling error: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("Message handling error: ") + e.what());
        }
    }
}

void KrakenConnector::on_open(ConnectionHdl hdl) {
    state_.store(ConnectionState::CONNECTED);
    reconnect_count_.store(0);
    spdlog::info("KrakenConnector: Connected to Kraken WebSocket");

    if (connection_callback_) {
        connection_callback_(true, "Connected");
    }

    // Resubscribe to all pairs
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    for (const auto& symbol : subscriptions_) {
        send_subscription(symbol, true);
    }
}

void KrakenConnector::on_close(ConnectionHdl hdl) {
    auto prev_state = state_.exchange(ConnectionState::DISCONNECTED);
    spdlog::info("KrakenConnector: Connection closed");

    if (connection_callback_) {
        connection_callback_(false, "Connection closed");
    }

    // Auto-reconnect if enabled
    if (auto_reconnect_ && prev_state == ConnectionState::CONNECTED) {
        uint32_t attempts = reconnect_count_.fetch_add(1);
        if (max_reconnect_attempts_ == 0 || attempts < max_reconnect_attempts_) {
            spdlog::info("KrakenConnector: Attempting reconnect in {}ms (attempt {})",
                        reconnect_delay_ms_, attempts + 1);

            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms_));

            state_.store(ConnectionState::RECONNECTING);
            connect();
        } else {
            spdlog::error("KrakenConnector: Max reconnect attempts reached");
            state_.store(ConnectionState::FAILED);
        }
    }
}

void KrakenConnector::on_fail(ConnectionHdl hdl) {
    state_.store(ConnectionState::FAILED);
    spdlog::error("KrakenConnector: Connection failed");

    if (connection_callback_) {
        connection_callback_(false, "Connection failed");
    }

    // Try to get error reason
    try {
        auto con = client_.get_con_from_hdl(hdl);
        auto ec = con->get_ec();
        spdlog::error("KrakenConnector: Error code: {}", ec.message());

        if (error_callback_) {
            error_callback_("Connection failed: " + ec.message());
        }
    } catch (...) {
        if (error_callback_) {
            error_callback_("Connection failed");
        }
    }
}

KrakenConnector::ContextPtr KrakenConnector::on_tls_init(ConnectionHdl hdl) {
    auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::tlsv12_client);

    try {
        ctx->set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::single_dh_use);

    } catch (const std::exception& e) {
        spdlog::error("KrakenConnector: TLS init error: {}", e.what());
    }

    return ctx;
}

std::optional<NormalizedOrderBookUpdate> KrakenConnector::parse_snapshot(
    const simdjson::dom::element& json, const std::string& pair) {

    try {
        NormalizedOrderBookUpdate update;
        update.exchange = Exchange::KRAKEN;
        update.timestamp = get_timestamp_ns();
        update.symbol = normalize_symbol(pair);

        // Parse bids ("bs" in snapshot)
        if (json["bs"].error() == simdjson::SUCCESS) {
            auto bids_array = json["bs"].get_array();
            for (auto bid_element : bids_array) {
                auto bid_array = bid_element.get_array();
                auto it = bid_array.begin();

                std::string_view price_str = (*it).get_string().value();
                ++it;
                std::string_view qty_str = (*it).get_string().value();

                PriceLevelSnapshot level;
                level.price = std::stod(std::string(price_str));
                level.quantity = std::stod(std::string(qty_str));
                level.order_count = 1;  // Kraken doesn't provide order count

                update.bids.push_back(level);
            }
        }

        // Parse asks ("as" in snapshot)
        if (json["as"].error() == simdjson::SUCCESS) {
            auto asks_array = json["as"].get_array();
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
        }

        spdlog::debug("KrakenConnector: Parsed snapshot for {} ({} bids, {} asks)",
                     update.symbol, update.bids.size(), update.asks.size());

        return update;

    } catch (const std::exception& e) {
        spdlog::error("KrakenConnector: Snapshot parse error: {}", e.what());
        return std::nullopt;
    }
}

std::optional<NormalizedOrderBookUpdate> KrakenConnector::parse_update(
    const simdjson::dom::element& json, const std::string& pair) {

    try {
        NormalizedOrderBookUpdate update;
        update.exchange = Exchange::KRAKEN;
        update.timestamp = get_timestamp_ns();
        update.symbol = normalize_symbol(pair);

        // Parse bids ("b" in update)
        if (json["b"].error() == simdjson::SUCCESS) {
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
                level.order_count = 1;

                update.bids.push_back(level);
            }
        }

        // Parse asks ("a" in update)
        if (json["a"].error() == simdjson::SUCCESS) {
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
        }

        return update;

    } catch (const std::exception& e) {
        spdlog::error("KrakenConnector: Update parse error: {}", e.what());
        return std::nullopt;
    }
}

void KrakenConnector::send_subscription(const std::string& symbol, bool subscribe) {
    try {
        // Build subscription message
        std::string event = subscribe ? "subscribe" : "unsubscribe";

        // Build JSON message
        std::string message = "{"
            "\"event\":\"" + event + "\","
            "\"pair\":[\"" + symbol + "\"],"
            "\"subscription\":{\"name\":\"book\",\"depth\":" + std::to_string(depth_) + "}"
        "}";

        websocketpp::lib::error_code ec;
        client_.send(connection_, message, websocketpp::frame::opcode::text, ec);

        if (ec) {
            spdlog::error("KrakenConnector: Send subscription error: {}", ec.message());
            if (error_callback_) {
                error_callback_("Send error: " + ec.message());
            }
        } else {
            spdlog::debug("KrakenConnector: Sent {} for {}", event, symbol);
        }

    } catch (const std::exception& e) {
        spdlog::error("KrakenConnector: Subscription error: {}", e.what());
        if (error_callback_) {
            error_callback_(std::string("Subscription error: ") + e.what());
        }
    }
}

std::string KrakenConnector::normalize_symbol(const std::string& kraken_symbol) const {
    // Kraken uses XBT for Bitcoin, we might want to normalize to BTC
    // For now, just return as-is, but this can be customized
    return kraken_symbol;
}

} // namespace marketdata
