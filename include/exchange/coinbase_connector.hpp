/**
 * @file coinbase_connector.hpp
 * @brief Coinbase WebSocket connector implementation
 *
 * Implements WebSocket connector for Coinbase exchange. Connects to Coinbase
 * WebSocket feed and streams real-time Level 2 order book updates.
 *
 * **Coinbase WebSocket API:**
 * - Base URL: wss://ws-feed.exchange.coinbase.com
 * - Channel: level2_batch (batched updates every 50ms)
 * - Message format: JSON
 * - Snapshot provided: Yes (on subscription)
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "exchange/websocket_connector.hpp"
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <simdjson.h>
#include <thread>
#include <mutex>
#include <set>

namespace marketdata {

/**
 * @brief Coinbase WebSocket connector
 *
 * Connects to Coinbase WebSocket feed for real-time Level 2 market data.
 * Provides full order book snapshots on subscription and incremental updates.
 *
 * **Coinbase Specifics:**
 * - Symbol format: uppercase with hyphen (e.g., "BTC-USD")
 * - Snapshot: Full snapshot sent on subscription
 * - Updates: Batched updates every 50ms
 * - Rate limits: No explicit limit, but throttle to ~30 messages/sec
 *
 * **Message Types:**
 * - snapshot: Full order book on subscription
 * - l2update: Incremental updates (changes only)
 *
 * **Snapshot Message:**
 * ```json
 * {
 *   "type": "snapshot",
 *   "product_id": "BTC-USD",
 *   "bids": [["10101.10", "0.45054140"]],
 *   "asks": [["10102.55", "0.57753524"]]
 * }
 * ```
 *
 * **Update Message:**
 * ```json
 * {
 *   "type": "l2update",
 *   "product_id": "BTC-USD",
 *   "time": "2019-08-14T20:42:27.265Z",
 *   "changes": [
 *     ["buy", "10101.80000000", "0.162567"]
 *   ]
 * }
 * ```
 *
 * @code
 * // Create Coinbase connector
 * auto coinbase = std::make_unique<CoinbaseConnector>();
 *
 * // Set callbacks
 * coinbase->set_orderbook_callback([](const auto& update) {
 *     std::cout << "Coinbase " << update.symbol << " updated\n";
 * });
 *
 * // Connect and subscribe
 * coinbase->connect();
 * coinbase->subscribe("BTC-USD");  // Note: uppercase with hyphen
 *
 * // Run in background
 * std::thread io_thread([&]() { coinbase->run(); });
 *
 * // Cleanup
 * coinbase->stop();
 * io_thread.join();
 * coinbase->disconnect();
 * @endcode
 *
 * @note Symbol format is uppercase with hyphen separator
 * @note Snapshot is automatically provided on subscription
 * @see https://docs.cloud.coinbase.com/exchange/docs/websocket-overview
 */
class CoinbaseConnector : public WebSocketConnector {
public:
    /**
     * @brief Construct Coinbase connector
     *
     * @param use_sandbox If true, connect to sandbox (default: false)
     */
    explicit CoinbaseConnector(bool use_sandbox = false);

    /**
     * @brief Destructor
     *
     * Automatically disconnects if still connected.
     */
    ~CoinbaseConnector() override;

    /**
     * @brief Connect to Coinbase WebSocket feed
     *
     * Establishes connection to wss://ws-feed.exchange.coinbase.com
     *
     * @return bool True if connection initiated successfully
     *
     * @note Not thread-safe
     */
    bool connect() override;

    /**
     * @brief Disconnect from Coinbase
     *
     * Gracefully closes WebSocket connection.
     *
     * @note Not thread-safe
     */
    void disconnect() override;

    /**
     * @brief Run WebSocket I/O loop
     *
     * Blocks until stop() is called.
     *
     * @note Run in separate thread
     */
    void run() override;

    /**
     * @brief Stop I/O loop
     *
     * Signals run() to exit.
     *
     * @note Thread-safe
     */
    void stop() override;

    /**
     * @brief Subscribe to product's Level 2 updates
     *
     * Subscribes to level2_batch channel for a product. Receives
     * full snapshot immediately, then incremental updates.
     *
     * @param symbol Product ID in Coinbase format (e.g., "BTC-USD")
     * @return bool True if subscription sent successfully
     *
     * @code
     * coinbase->subscribe("BTC-USD");   // Bitcoin/USD
     * coinbase->subscribe("ETH-USD");   // Ethereum/USD
     * coinbase->subscribe("BTC-EUR");   // Bitcoin/Euro
     * @endcode
     *
     * @note Thread-safe
     * @note Symbol must be uppercase with hyphen
     * @note Snapshot is sent immediately after subscription
     */
    bool subscribe(const std::string& symbol) override;

    /**
     * @brief Unsubscribe from product
     *
     * @param symbol Product ID to unsubscribe
     * @return bool True if unsubscription sent successfully
     *
     * @note Thread-safe
     */
    bool unsubscribe(const std::string& symbol) override;

    /**
     * @brief Get WebSocket URI
     *
     * @return std::string WebSocket URI for Coinbase
     */
    std::string get_uri() const;

private:
    // WebSocket client type
    using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
    using MessagePtr = Client::message_ptr;
    using ConnectionHdl = websocketpp::connection_hdl;
    using ContextPtr = websocketpp::lib::shared_ptr<boost::asio::ssl::context>;

    /**
     * @brief Handle incoming WebSocket message
     *
     * Parses JSON message and handles snapshot/update messages.
     *
     * @param hdl Connection handle
     * @param msg Message payload
     */
    void on_message(ConnectionHdl hdl, MessagePtr msg);

    /**
     * @brief Handle connection opened
     *
     * @param hdl Connection handle
     */
    void on_open(ConnectionHdl hdl);

    /**
     * @brief Handle connection closed
     *
     * @param hdl Connection handle
     */
    void on_close(ConnectionHdl hdl);

    /**
     * @brief Handle connection failed
     *
     * @param hdl Connection handle
     */
    void on_fail(ConnectionHdl hdl);

    /**
     * @brief Handle TLS initialization
     *
     * @param hdl Connection handle
     * @return ContextPtr SSL context
     */
    ContextPtr on_tls_init(ConnectionHdl hdl);

    /**
     * @brief Parse Coinbase snapshot message
     *
     * Parses full order book snapshot.
     *
     * @param json JSON document
     * @return std::optional<NormalizedOrderBookUpdate> Parsed snapshot
     */
    std::optional<NormalizedOrderBookUpdate> parse_snapshot(
        const simdjson::dom::element& json);

    /**
     * @brief Parse Coinbase l2update message
     *
     * Parses incremental order book updates.
     *
     * @param json JSON document
     * @return std::optional<NormalizedOrderBookUpdate> Parsed update
     */
    std::optional<NormalizedOrderBookUpdate> parse_l2update(
        const simdjson::dom::element& json);

    /**
     * @brief Send subscription message
     *
     * Sends subscribe/unsubscribe request to Coinbase WebSocket.
     *
     * @param symbol Product ID to subscribe
     * @param subscribe True to subscribe, false to unsubscribe
     */
    void send_subscription(const std::string& symbol, bool subscribe);

    Client client_;                           ///< WebSocket client
    ConnectionHdl connection_;                ///< Connection handle
    std::string uri_;                         ///< WebSocket URI
    bool use_sandbox_;                        ///< Use sandbox flag

    simdjson::dom::parser parser_;            ///< JSON parser

    std::mutex subscriptions_mutex_;          ///< Protects subscriptions_
    std::set<std::string> subscriptions_;     ///< Subscribed products

    std::atomic<bool> running_{false};        ///< Running flag
    std::atomic<uint32_t> reconnect_count_{0}; ///< Reconnection attempt counter
};

} // namespace marketdata
