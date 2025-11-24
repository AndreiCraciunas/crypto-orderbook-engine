/**
 * @file connection_manager.cpp
 * @brief Connection manager implementation
 */

#include "exchange/connection_manager.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace marketdata {

ConnectionManager::ConnectionManager() {
    spdlog::info("ConnectionManager: Created");
}

ConnectionManager::~ConnectionManager() {
    spdlog::info("ConnectionManager: Shutting down");
    stop_all();
}

bool ConnectionManager::add_exchange(Exchange exchange, uint32_t depth, bool use_testnet) {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    // Check if already exists
    if (connectors_.find(exchange) != connectors_.end()) {
        spdlog::warn("ConnectionManager: Exchange {} already added", static_cast<int>(exchange));
        return false;
    }

    // Create connector
    auto connector = create_connector(exchange, depth, use_testnet);
    if (!connector) {
        spdlog::error("ConnectionManager: Failed to create connector for exchange {}",
                     static_cast<int>(exchange));
        return false;
    }

    // Set callbacks
    connector->set_orderbook_callback([this](const NormalizedOrderBookUpdate& update) {
        if (orderbook_callback_) {
            orderbook_callback_(update);
        }
    });

    connector->set_connection_callback([this, exchange](bool connected, const std::string& msg) {
        if (connection_callback_) {
            connection_callback_(exchange, connected, msg);
        }
    });

    connector->set_error_callback([this, exchange](const std::string& error) {
        if (error_callback_) {
            error_callback_(exchange, error);
        }
    });

    // Set auto-reconnect settings
    connector->set_auto_reconnect(auto_reconnect_.load(),
                                  max_reconnect_attempts_.load(),
                                  reconnect_delay_ms_.load());

    // Store connector
    ConnectorInfo info;
    info.connector = std::move(connector);
    connectors_[exchange] = std::move(info);

    spdlog::info("ConnectionManager: Added exchange {}", static_cast<int>(exchange));
    return true;
}

bool ConnectionManager::remove_exchange(Exchange exchange) {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    auto it = connectors_.find(exchange);
    if (it == connectors_.end()) {
        spdlog::warn("ConnectionManager: Exchange {} not found", static_cast<int>(exchange));
        return false;
    }

    // Stop if running
    if (it->second.running.load()) {
        it->second.connector->stop();
        it->second.running.store(false);

        if (it->second.io_thread && it->second.io_thread->joinable()) {
            it->second.io_thread->join();
        }

        it->second.connector->disconnect();
    }

    connectors_.erase(it);
    spdlog::info("ConnectionManager: Removed exchange {}", static_cast<int>(exchange));
    return true;
}

bool ConnectionManager::start_all() {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    bool all_success = true;
    for (auto& [exchange, info] : connectors_) {
        if (info.running.load()) {
            spdlog::debug("ConnectionManager: Exchange {} already running",
                         static_cast<int>(exchange));
            continue;
        }

        // Connect
        if (!info.connector->connect()) {
            spdlog::error("ConnectionManager: Failed to connect to exchange {}",
                         static_cast<int>(exchange));
            all_success = false;
            continue;
        }

        // Start I/O thread
        info.running.store(true);
        info.io_thread = std::make_unique<std::thread>([&info, exchange]() {
            spdlog::debug("ConnectionManager: I/O thread started for exchange {}",
                         static_cast<int>(exchange));
            info.connector->run();
            info.running.store(false);
            spdlog::debug("ConnectionManager: I/O thread stopped for exchange {}",
                         static_cast<int>(exchange));
        });

        spdlog::info("ConnectionManager: Started exchange {}", static_cast<int>(exchange));
    }

    return all_success;
}

bool ConnectionManager::start(Exchange exchange) {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    auto it = connectors_.find(exchange);
    if (it == connectors_.end()) {
        spdlog::warn("ConnectionManager: Exchange {} not found", static_cast<int>(exchange));
        return false;
    }

    if (it->second.running.load()) {
        spdlog::debug("ConnectionManager: Exchange {} already running",
                     static_cast<int>(exchange));
        return true;
    }

    // Connect
    if (!it->second.connector->connect()) {
        spdlog::error("ConnectionManager: Failed to connect to exchange {}",
                     static_cast<int>(exchange));
        return false;
    }

    // Start I/O thread
    it->second.running.store(true);
    it->second.io_thread = std::make_unique<std::thread>([&info = it->second, exchange]() {
        spdlog::debug("ConnectionManager: I/O thread started for exchange {}",
                     static_cast<int>(exchange));
        info.connector->run();
        info.running.store(false);
        spdlog::debug("ConnectionManager: I/O thread stopped for exchange {}",
                     static_cast<int>(exchange));
    });

    spdlog::info("ConnectionManager: Started exchange {}", static_cast<int>(exchange));
    return true;
}

void ConnectionManager::stop_all() {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    for (auto& [exchange, info] : connectors_) {
        if (!info.running.load()) {
            continue;
        }

        spdlog::info("ConnectionManager: Stopping exchange {}", static_cast<int>(exchange));

        // Stop I/O loop
        info.connector->stop();
        info.running.store(false);

        // Join thread
        if (info.io_thread && info.io_thread->joinable()) {
            info.io_thread->join();
        }

        // Disconnect
        info.connector->disconnect();

        spdlog::info("ConnectionManager: Stopped exchange {}", static_cast<int>(exchange));
    }
}

void ConnectionManager::stop(Exchange exchange) {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    auto it = connectors_.find(exchange);
    if (it == connectors_.end()) {
        spdlog::warn("ConnectionManager: Exchange {} not found", static_cast<int>(exchange));
        return;
    }

    if (!it->second.running.load()) {
        spdlog::debug("ConnectionManager: Exchange {} not running",
                     static_cast<int>(exchange));
        return;
    }

    spdlog::info("ConnectionManager: Stopping exchange {}", static_cast<int>(exchange));

    // Stop I/O loop
    it->second.connector->stop();
    it->second.running.store(false);

    // Join thread
    if (it->second.io_thread && it->second.io_thread->joinable()) {
        it->second.io_thread->join();
    }

    // Disconnect
    it->second.connector->disconnect();

    spdlog::info("ConnectionManager: Stopped exchange {}", static_cast<int>(exchange));
}

size_t ConnectionManager::subscribe_all(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    size_t count = 0;
    for (auto& [exchange, info] : connectors_) {
        if (!info.connector->is_connected()) {
            continue;
        }

        if (info.connector->subscribe(symbol)) {
            count++;
        }
    }

    spdlog::info("ConnectionManager: Subscribed to {} on {} exchanges", symbol, count);
    return count;
}

bool ConnectionManager::subscribe(Exchange exchange, const std::string& symbol) {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    auto it = connectors_.find(exchange);
    if (it == connectors_.end()) {
        spdlog::warn("ConnectionManager: Exchange {} not found", static_cast<int>(exchange));
        return false;
    }

    if (!it->second.connector->is_connected()) {
        spdlog::warn("ConnectionManager: Exchange {} not connected", static_cast<int>(exchange));
        return false;
    }

    bool success = it->second.connector->subscribe(symbol);
    if (success) {
        spdlog::info("ConnectionManager: Subscribed to {} on exchange {}",
                    symbol, static_cast<int>(exchange));
    }

    return success;
}

size_t ConnectionManager::unsubscribe_all(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    size_t count = 0;
    for (auto& [exchange, info] : connectors_) {
        if (!info.connector->is_connected()) {
            continue;
        }

        if (info.connector->unsubscribe(symbol)) {
            count++;
        }
    }

    spdlog::info("ConnectionManager: Unsubscribed from {} on {} exchanges", symbol, count);
    return count;
}

bool ConnectionManager::unsubscribe(Exchange exchange, const std::string& symbol) {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    auto it = connectors_.find(exchange);
    if (it == connectors_.end()) {
        spdlog::warn("ConnectionManager: Exchange {} not found", static_cast<int>(exchange));
        return false;
    }

    if (!it->second.connector->is_connected()) {
        spdlog::warn("ConnectionManager: Exchange {} not connected", static_cast<int>(exchange));
        return false;
    }

    bool success = it->second.connector->unsubscribe(symbol);
    if (success) {
        spdlog::info("ConnectionManager: Unsubscribed from {} on exchange {}",
                    symbol, static_cast<int>(exchange));
    }

    return success;
}

void ConnectionManager::set_orderbook_callback(OrderBookUpdateCallback callback) {
    orderbook_callback_ = callback;
}

void ConnectionManager::set_connection_callback(
    std::function<void(Exchange, bool, const std::string&)> callback) {
    connection_callback_ = callback;
}

void ConnectionManager::set_error_callback(
    std::function<void(Exchange, const std::string&)> callback) {
    error_callback_ = callback;
}

void ConnectionManager::set_auto_reconnect(bool enable, uint32_t max_attempts,
                                          uint32_t delay_ms) {
    auto_reconnect_.store(enable);
    max_reconnect_attempts_.store(max_attempts);
    reconnect_delay_ms_.store(delay_ms);

    // Update existing connectors
    std::lock_guard<std::mutex> lock(connectors_mutex_);
    for (auto& [exchange, info] : connectors_) {
        info.connector->set_auto_reconnect(enable, max_attempts, delay_ms);
    }

    spdlog::info("ConnectionManager: Auto-reconnect set to {} (max: {}, delay: {}ms)",
                enable, max_attempts, delay_ms);
}

bool ConnectionManager::is_connected(Exchange exchange) const {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    auto it = connectors_.find(exchange);
    if (it == connectors_.end()) {
        return false;
    }

    return it->second.connector->is_connected();
}

ConnectionState ConnectionManager::get_state(Exchange exchange) const {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    auto it = connectors_.find(exchange);
    if (it == connectors_.end()) {
        return ConnectionState::DISCONNECTED;
    }

    return it->second.connector->get_state();
}

size_t ConnectionManager::get_active_connections() const {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    return std::count_if(connectors_.begin(), connectors_.end(),
        [](const auto& pair) {
            return pair.second.connector->is_connected();
        });
}

std::vector<Exchange> ConnectionManager::get_exchanges() const {
    std::lock_guard<std::mutex> lock(connectors_mutex_);

    std::vector<Exchange> exchanges;
    exchanges.reserve(connectors_.size());

    for (const auto& [exchange, _] : connectors_) {
        exchanges.push_back(exchange);
    }

    return exchanges;
}

std::unique_ptr<WebSocketConnector> ConnectionManager::create_connector(
    Exchange exchange, uint32_t depth, bool use_testnet) {

    switch (exchange) {
        case Exchange::BINANCE:
            return std::make_unique<BinanceConnector>(use_testnet);

        case Exchange::COINBASE:
            return std::make_unique<CoinbaseConnector>(use_testnet);

        case Exchange::KRAKEN:
            return std::make_unique<KrakenConnector>(depth);

        default:
            spdlog::error("ConnectionManager: Unknown exchange {}", static_cast<int>(exchange));
            return nullptr;
    }
}

} // namespace marketdata
