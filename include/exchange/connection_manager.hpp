/**
 * @file connection_manager.hpp
 * @brief Connection manager for multiple exchange connectors
 *
 * Manages lifecycle and coordination of multiple exchange WebSocket connectors.
 * Provides centralized control for connecting, subscribing, and handling updates
 * from multiple exchanges simultaneously.
 *
 * **Features:**
 * - Multi-exchange connection management
 * - Unified callback interface
 * - Automatic reconnection handling
 * - Thread-safe operations
 * - Graceful shutdown
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "exchange/websocket_connector.hpp"
#include "exchange/binance_connector.hpp"
#include "exchange/coinbase_connector.hpp"
#include "exchange/kraken_connector.hpp"
#include <memory>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <atomic>

namespace marketdata {

/**
 * @brief Connection manager for multiple exchanges
 *
 * Manages multiple WebSocket connectors for different exchanges. Provides
 * unified interface for lifecycle management, subscriptions, and callbacks.
 *
 * **Architecture:**
 * - One connector per exchange
 * - Each connector runs in separate thread
 * - Callbacks aggregated to single handler
 * - Thread-safe subscription management
 *
 * **Lifecycle:**
 * 1. Create manager
 * 2. Add connectors for desired exchanges
 * 3. Set callbacks
 * 4. Start all connections
 * 5. Subscribe to symbols
 * 6. Receive updates via callbacks
 * 7. Stop all connections
 *
 * @code
 * // Create connection manager
 * auto manager = std::make_unique<ConnectionManager>();
 *
 * // Add exchanges
 * manager->add_exchange(Exchange::BINANCE);
 * manager->add_exchange(Exchange::COINBASE);
 * manager->add_exchange(Exchange::KRAKEN, 25);  // depth 25
 *
 * // Set callback
 * manager->set_orderbook_callback([](const auto& update) {
 *     std::cout << "Update from " << exchange_name(update.exchange)
 *               << " for " << update.symbol << "\n";
 * });
 *
 * // Start all connections
 * manager->start_all();
 *
 * // Subscribe to symbols (all exchanges)
 * manager->subscribe_all("BTC-USD");  // Normalized symbol
 *
 * // Or subscribe to specific exchange
 * manager->subscribe(Exchange::BINANCE, "btcusdt");
 *
 * // Wait for data...
 * std::this_thread::sleep_for(std::chrono::seconds(60));
 *
 * // Clean shutdown
 * manager->stop_all();
 * @endcode
 *
 * @note All public methods are thread-safe
 * @note Connectors run in separate threads automatically
 */
class ConnectionManager {
public:
    /**
     * @brief Construct connection manager
     */
    ConnectionManager();

    /**
     * @brief Destructor
     *
     * Automatically stops all connections and joins threads.
     */
    ~ConnectionManager();

    // Disable copy and move
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;
    ConnectionManager(ConnectionManager&&) = delete;
    ConnectionManager& operator=(ConnectionManager&&) = delete;

    /**
     * @brief Add exchange connector
     *
     * Creates and registers a connector for the specified exchange.
     * Must be called before start_all().
     *
     * @param exchange Exchange to add
     * @param depth Order book depth (Kraken only, default: 10)
     * @param use_testnet Use testnet/sandbox (Binance/Coinbase, default: false)
     * @return bool True if connector added successfully
     *
     * @code
     * manager->add_exchange(Exchange::BINANCE);
     * manager->add_exchange(Exchange::COINBASE, 0, false);
     * manager->add_exchange(Exchange::KRAKEN, 100);  // depth 100
     * @endcode
     *
     * @note Thread-safe
     * @note Cannot add same exchange twice
     * @warning Must be called before start_all()
     */
    bool add_exchange(Exchange exchange, uint32_t depth = 10, bool use_testnet = false);

    /**
     * @brief Remove exchange connector
     *
     * Stops and removes the connector for the specified exchange.
     *
     * @param exchange Exchange to remove
     * @return bool True if connector removed successfully
     *
     * @note Thread-safe
     * @note Automatically stops connector if running
     */
    bool remove_exchange(Exchange exchange);

    /**
     * @brief Start all exchange connections
     *
     * Connects to all registered exchanges and starts I/O threads.
     * Blocks until all connections are initiated (not necessarily connected).
     *
     * @return bool True if all connections started successfully
     *
     * @code
     * manager->add_exchange(Exchange::BINANCE);
     * manager->add_exchange(Exchange::COINBASE);
     * manager->start_all();  // Connects to both
     * @endcode
     *
     * @note Thread-safe
     * @note Creates one I/O thread per exchange
     * @note Returns immediately after initiating connections
     */
    bool start_all();

    /**
     * @brief Start specific exchange connection
     *
     * Connects to a specific exchange and starts its I/O thread.
     *
     * @param exchange Exchange to start
     * @return bool True if connection started successfully
     *
     * @note Thread-safe
     */
    bool start(Exchange exchange);

    /**
     * @brief Stop all exchange connections
     *
     * Gracefully disconnects from all exchanges and joins I/O threads.
     * Blocks until all threads have stopped.
     *
     * @note Thread-safe
     * @note Blocks until all I/O threads terminate
     */
    void stop_all();

    /**
     * @brief Stop specific exchange connection
     *
     * Gracefully disconnects from a specific exchange and joins its thread.
     *
     * @param exchange Exchange to stop
     *
     * @note Thread-safe
     * @note Blocks until I/O thread terminates
     */
    void stop(Exchange exchange);

    /**
     * @brief Subscribe to symbol on all exchanges
     *
     * Subscribes to a symbol on all connected exchanges. Symbol format
     * is normalized (user must provide exchange-specific format).
     *
     * @param symbol Symbol to subscribe (exchange-specific format)
     * @return size_t Number of exchanges subscribed successfully
     *
     * @code
     * // Must provide map of exchange-specific symbols
     * manager->subscribe_all("btcusdt");  // Won't work for all
     *
     * // Better approach:
     * manager->subscribe(Exchange::BINANCE, "btcusdt");
     * manager->subscribe(Exchange::COINBASE, "BTC-USD");
     * manager->subscribe(Exchange::KRAKEN, "XBT/USD");
     * @endcode
     *
     * @note Thread-safe
     * @note Only subscribes to currently connected exchanges
     * @warning Symbol format is exchange-specific
     */
    size_t subscribe_all(const std::string& symbol);

    /**
     * @brief Subscribe to symbol on specific exchange
     *
     * Subscribes to a symbol on a specific exchange using that
     * exchange's symbol format.
     *
     * @param exchange Exchange to subscribe
     * @param symbol Symbol in exchange-specific format
     * @return bool True if subscription sent successfully
     *
     * @code
     * manager->subscribe(Exchange::BINANCE, "btcusdt");
     * manager->subscribe(Exchange::COINBASE, "BTC-USD");
     * manager->subscribe(Exchange::KRAKEN, "XBT/USD");
     * @endcode
     *
     * @note Thread-safe
     * @note Symbol format must match exchange requirements:
     *       - Binance: lowercase, no separator (btcusdt)
     *       - Coinbase: uppercase with hyphen (BTC-USD)
     *       - Kraken: uppercase with slash (XBT/USD)
     */
    bool subscribe(Exchange exchange, const std::string& symbol);

    /**
     * @brief Unsubscribe from symbol on all exchanges
     *
     * Unsubscribes from a symbol on all connected exchanges.
     *
     * @param symbol Symbol to unsubscribe
     * @return size_t Number of exchanges unsubscribed successfully
     *
     * @note Thread-safe
     */
    size_t unsubscribe_all(const std::string& symbol);

    /**
     * @brief Unsubscribe from symbol on specific exchange
     *
     * Unsubscribes from a symbol on a specific exchange.
     *
     * @param exchange Exchange to unsubscribe
     * @param symbol Symbol to unsubscribe
     * @return bool True if unsubscription sent successfully
     *
     * @note Thread-safe
     */
    bool unsubscribe(Exchange exchange, const std::string& symbol);

    /**
     * @brief Set order book update callback
     *
     * Sets callback for all order book updates from all exchanges.
     * Callback is invoked from I/O threads (one per exchange).
     *
     * @param callback Callback function
     *
     * @code
     * manager->set_orderbook_callback([](const auto& update) {
     *     std::cout << exchange_name(update.exchange) << ": "
     *               << update.symbol << " updated\n";
     * });
     * @endcode
     *
     * @note Thread-safe (callback setting)
     * @warning Callback is invoked from multiple threads concurrently
     * @warning Callback must be thread-safe or use synchronization
     */
    void set_orderbook_callback(OrderBookUpdateCallback callback);

    /**
     * @brief Set connection state callback
     *
     * Sets callback for connection state changes from all exchanges.
     *
     * @param callback Callback function
     *
     * @code
     * manager->set_connection_callback([](Exchange ex, bool connected, const std::string& msg) {
     *     std::cout << exchange_name(ex) << " "
     *               << (connected ? "connected" : "disconnected")
     *               << ": " << msg << "\n";
     * });
     * @endcode
     *
     * @note Thread-safe (callback setting)
     * @warning Callback is invoked from multiple threads concurrently
     */
    void set_connection_callback(
        std::function<void(Exchange, bool, const std::string&)> callback);

    /**
     * @brief Set error callback
     *
     * Sets callback for errors from all exchanges.
     *
     * @param callback Callback function
     *
     * @code
     * manager->set_error_callback([](Exchange ex, const std::string& error) {
     *     std::cerr << exchange_name(ex) << " error: " << error << "\n";
     * });
     * @endcode
     *
     * @note Thread-safe (callback setting)
     * @warning Callback is invoked from multiple threads concurrently
     */
    void set_error_callback(
        std::function<void(Exchange, const std::string&)> callback);

    /**
     * @brief Enable/disable auto-reconnect for all exchanges
     *
     * @param enable True to enable auto-reconnect
     * @param max_attempts Max reconnect attempts (0 = unlimited)
     * @param delay_ms Delay between attempts in milliseconds
     *
     * @note Thread-safe
     * @note Applies to all current and future connectors
     */
    void set_auto_reconnect(bool enable, uint32_t max_attempts = 0,
                           uint32_t delay_ms = 5000);

    /**
     * @brief Check if exchange is connected
     *
     * @param exchange Exchange to check
     * @return bool True if exchange is connected
     *
     * @note Thread-safe
     */
    bool is_connected(Exchange exchange) const;

    /**
     * @brief Get connection state for exchange
     *
     * @param exchange Exchange to check
     * @return ConnectionState Connection state
     *
     * @note Thread-safe
     */
    ConnectionState get_state(Exchange exchange) const;

    /**
     * @brief Get number of active connections
     *
     * @return size_t Number of exchanges in CONNECTED state
     *
     * @note Thread-safe
     */
    size_t get_active_connections() const;

    /**
     * @brief Get list of registered exchanges
     *
     * @return std::vector<Exchange> List of registered exchanges
     *
     * @note Thread-safe
     */
    std::vector<Exchange> get_exchanges() const;

private:
    /**
     * @brief Connector with thread
     */
    struct ConnectorInfo {
        std::unique_ptr<WebSocketConnector> connector;
        std::unique_ptr<std::thread> io_thread;
        std::atomic<bool> running{false};
    };

    mutable std::mutex connectors_mutex_;                  ///< Protects connectors_
    std::map<Exchange, ConnectorInfo> connectors_;         ///< Exchange -> Connector map

    OrderBookUpdateCallback orderbook_callback_;           ///< Order book callback
    std::function<void(Exchange, bool, const std::string&)> connection_callback_; ///< Connection callback
    std::function<void(Exchange, const std::string&)> error_callback_; ///< Error callback

    std::atomic<bool> auto_reconnect_{true};               ///< Auto-reconnect flag
    std::atomic<uint32_t> max_reconnect_attempts_{0};      ///< Max reconnect attempts
    std::atomic<uint32_t> reconnect_delay_ms_{5000};       ///< Reconnect delay

    /**
     * @brief Create connector for exchange
     *
     * Factory method to create exchange-specific connector.
     *
     * @param exchange Exchange type
     * @param depth Order book depth (Kraken only)
     * @param use_testnet Use testnet/sandbox
     * @return std::unique_ptr<WebSocketConnector> Created connector
     */
    std::unique_ptr<WebSocketConnector> create_connector(
        Exchange exchange, uint32_t depth, bool use_testnet);
};

} // namespace marketdata
