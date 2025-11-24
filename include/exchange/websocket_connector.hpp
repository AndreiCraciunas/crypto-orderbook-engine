/**
 * @file websocket_connector.hpp
 * @brief Base WebSocket connector interface for exchange connections
 *
 * Defines abstract base class for exchange-specific WebSocket connectors.
 * Handles connection lifecycle, message callbacks, and reconnection logic.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "core/types.hpp"
#include <functional>
#include <string>
#include <memory>
#include <atomic>

namespace marketdata {

/**
 * @brief Callback types for WebSocket events
 */

/// Called when order book update is received
using OrderBookUpdateCallback = std::function<void(const NormalizedOrderBookUpdate&)>;

/// Called when trade is received
using TradeCallback = std::function<void(const Trade&)>;

/// Called when connection state changes
using ConnectionStateCallback = std::function<void(bool connected, const std::string& reason)>;

/// Called when error occurs
using ErrorCallback = std::function<void(const std::string& error_message)>;

/**
 * @brief Connection state enum
 */
enum class ConnectionState {
    DISCONNECTED,   ///< Not connected
    CONNECTING,     ///< Connection in progress
    CONNECTED,      ///< Connected and ready
    RECONNECTING,   ///< Reconnection attempt in progress
    FAILED          ///< Connection failed permanently
};

/**
 * @brief Abstract base class for exchange WebSocket connectors
 *
 * Provides common interface for connecting to exchange WebSocket APIs,
 * subscribing to market data streams, and handling reconnection logic.
 *
 * **Lifecycle:**
 * 1. Create connector with exchange-specific configuration
 * 2. Register callbacks for updates, trades, connection state
 * 3. Call connect() to establish connection
 * 4. Call subscribe() to subscribe to symbols
 * 5. Receive data via callbacks
 * 6. Call disconnect() to gracefully close
 *
 * **Thread Safety:**
 * - connect()/disconnect() are not thread-safe
 * - Callbacks are invoked from WebSocket I/O thread
 * - subscribe()/unsubscribe() are thread-safe
 *
 * @code
 * // Create connector (subclass)
 * auto connector = std::make_unique<BinanceConnector>();
 *
 * // Register callbacks
 * connector->set_orderbook_callback([](const auto& update) {
 *     std::cout << "Update: " << update.symbol << '\n';
 * });
 *
 * connector->set_connection_callback([](bool connected, const std::string& reason) {
 *     if (connected) {
 *         std::cout << "Connected!\n";
 *     } else {
 *         std::cout << "Disconnected: " << reason << '\n';
 *     }
 * });
 *
 * // Connect and subscribe
 * connector->connect();
 * connector->subscribe("BTC-USDT");
 *
 * // Run until stopped
 * connector->run();
 *
 * // Cleanup
 * connector->disconnect();
 * @endcode
 *
 * @note Subclasses must implement exchange-specific message parsing
 * @see BinanceConnector, CoinbaseConnector, KrakenConnector
 */
class WebSocketConnector {
public:
    /**
     * @brief Construct connector for specific exchange
     *
     * @param exchange Exchange identifier
     */
    explicit WebSocketConnector(Exchange exchange)
        : exchange_(exchange)
        , state_(ConnectionState::DISCONNECTED)
        , auto_reconnect_(true)
        , reconnect_delay_ms_(1000)
        , max_reconnect_attempts_(10) {}

    /**
     * @brief Virtual destructor
     */
    virtual ~WebSocketConnector() = default;

    // Non-copyable, non-movable
    WebSocketConnector(const WebSocketConnector&) = delete;
    WebSocketConnector& operator=(const WebSocketConnector&) = delete;
    WebSocketConnector(WebSocketConnector&&) = delete;
    WebSocketConnector& operator=(WebSocketConnector&&) = delete;

    /**
     * @brief Connect to exchange WebSocket API
     *
     * Establishes WebSocket connection to exchange. Returns immediately
     * after initiating connection. Use connection callback to detect
     * when connection is ready.
     *
     * @return bool True if connection initiated, false on error
     *
     * @code
     * if (connector->connect()) {
     *     std::cout << "Connecting...\n";
     * }
     * @endcode
     *
     * @note Not thread-safe
     * @see disconnect()
     * @see set_connection_callback()
     */
    virtual bool connect() = 0;

    /**
     * @brief Disconnect from exchange
     *
     * Gracefully closes WebSocket connection and unsubscribes from
     * all symbols.
     *
     * @code
     * connector->disconnect();
     * @endcode
     *
     * @note Not thread-safe
     * @note Blocks until connection is closed
     * @see connect()
     */
    virtual void disconnect() = 0;

    /**
     * @brief Run I/O event loop
     *
     * Runs WebSocket I/O loop. Blocks until stop() is called or
     * connection fails.
     *
     * @code
     * std::thread io_thread([&]() {
     *     connector->run();
     * });
     * @endcode
     *
     * @note Blocks - run in separate thread
     * @see stop()
     */
    virtual void run() = 0;

    /**
     * @brief Stop I/O event loop
     *
     * Signals run() to exit. Does not disconnect automatically.
     *
     * @code
     * connector->stop();  // Signal to exit
     * io_thread.join();   // Wait for run() to return
     * @endcode
     *
     * @note Thread-safe
     * @see run()
     */
    virtual void stop() = 0;

    /**
     * @brief Subscribe to symbol's order book updates
     *
     * Subscribes to real-time order book updates for a symbol.
     * Subscription is exchange-specific (format varies).
     *
     * @param symbol Symbol to subscribe (exchange-specific format)
     * @return bool True if subscription sent, false on error
     *
     * @code
     * // Binance format
     * connector->subscribe("btcusdt");
     *
     * // Coinbase format
     * connector->subscribe("BTC-USD");
     *
     * // Kraken format
     * connector->subscribe("XBT/USD");
     * @endcode
     *
     * @note Thread-safe
     * @note Format varies by exchange
     * @see unsubscribe()
     */
    virtual bool subscribe(const std::string& symbol) = 0;

    /**
     * @brief Unsubscribe from symbol
     *
     * Stops receiving updates for a symbol.
     *
     * @param symbol Symbol to unsubscribe
     * @return bool True if unsubscription sent, false on error
     *
     * @note Thread-safe
     * @see subscribe()
     */
    virtual bool unsubscribe(const std::string& symbol) = 0;

    /**
     * @brief Set order book update callback
     *
     * @param callback Function to call on updates
     *
     * @code
     * connector->set_orderbook_callback([](const auto& update) {
     *     for (const auto& bid : update.bids) {
     *         std::cout << "Bid: " << bid.price << " x " << bid.quantity << '\n';
     *     }
     * });
     * @endcode
     *
     * @note Callback invoked from I/O thread
     * @warning Keep callback fast to avoid blocking
     */
    void set_orderbook_callback(OrderBookUpdateCallback callback) {
        orderbook_callback_ = std::move(callback);
    }

    /**
     * @brief Set trade callback
     *
     * @param callback Function to call on trades
     *
     * @note Callback invoked from I/O thread
     */
    void set_trade_callback(TradeCallback callback) {
        trade_callback_ = std::move(callback);
    }

    /**
     * @brief Set connection state callback
     *
     * @param callback Function to call on connection state changes
     *
     * @note Callback invoked from I/O thread
     */
    void set_connection_callback(ConnectionStateCallback callback) {
        connection_callback_ = std::move(callback);
    }

    /**
     * @brief Set error callback
     *
     * @param callback Function to call on errors
     *
     * @note Callback invoked from I/O thread
     */
    void set_error_callback(ErrorCallback callback) {
        error_callback_ = std::move(callback);
    }

    /**
     * @brief Get current connection state
     *
     * @return ConnectionState Current state
     */
    ConnectionState get_state() const {
        return state_.load(std::memory_order_acquire);
    }

    /**
     * @brief Check if connected
     *
     * @return bool True if connected and ready
     */
    bool is_connected() const {
        return get_state() == ConnectionState::CONNECTED;
    }

    /**
     * @brief Get exchange identifier
     *
     * @return Exchange Exchange this connector is for
     */
    Exchange get_exchange() const {
        return exchange_;
    }

    /**
     * @brief Enable/disable automatic reconnection
     *
     * @param enable True to enable auto-reconnect
     */
    void set_auto_reconnect(bool enable) {
        auto_reconnect_ = enable;
    }

    /**
     * @brief Set reconnection delay
     *
     * @param delay_ms Delay in milliseconds between reconnect attempts
     */
    void set_reconnect_delay(uint32_t delay_ms) {
        reconnect_delay_ms_ = delay_ms;
    }

    /**
     * @brief Set maximum reconnection attempts
     *
     * @param max_attempts Maximum number of reconnect attempts (0 = unlimited)
     */
    void set_max_reconnect_attempts(uint32_t max_attempts) {
        max_reconnect_attempts_ = max_attempts;
    }

protected:
    Exchange exchange_;                           ///< Exchange identifier
    std::atomic<ConnectionState> state_;          ///< Current connection state

    // Callbacks
    OrderBookUpdateCallback orderbook_callback_;  ///< Order book update callback
    TradeCallback trade_callback_;                ///< Trade callback
    ConnectionStateCallback connection_callback_; ///< Connection state callback
    ErrorCallback error_callback_;                ///< Error callback

    // Reconnection settings
    bool auto_reconnect_;                         ///< Auto-reconnect enabled
    uint32_t reconnect_delay_ms_;                 ///< Delay between reconnects (ms)
    uint32_t max_reconnect_attempts_;             ///< Max reconnect attempts
};

} // namespace marketdata
