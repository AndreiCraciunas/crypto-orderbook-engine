/**
 * @file kraken_connector.hpp
 * @brief Kraken WebSocket connector implementation
 *
 * Implements WebSocket connector for Kraken exchange. Connects to Kraken
 * WebSocket API and streams real-time order book updates.
 *
 * **Kraken WebSocket API:**
 * - Base URL: wss://ws.kraken.com
 * - Channel: book (order book updates)
 * - Depth: 10, 25, 100, 500, 1000 levels
 * - Message format: JSON (array-based)
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
#include <map>

namespace marketdata {

/**
 * @brief Kraken WebSocket connector
 *
 * Connects to Kraken WebSocket API for real-time order book data.
 * Supports configurable depth levels and provides both snapshots and updates.
 *
 * **Kraken Specifics:**
 * - Symbol format: uppercase with slash (e.g., "XBT/USD")
 * - Depth options: 10, 25, 100, 500, 1000 levels
 * - Snapshot: Provided on subscription
 * - Updates: Incremental updates after snapshot
 * - Message format: JSON arrays (not objects)
 *
 * **Subscription Message:**
 * ```json
 * {
 *   "event": "subscribe",
 *   "pair": ["XBT/USD"],
 *   "subscription": {"name": "book", "depth": 10}
 * }
 * ```
 *
 * **Snapshot Message:**
 * ```json
 * [
 *   0,
 *   {
 *     "as": [["5541.30000", "2.50700000", "1534614248.123678"]],
 *     "bs": [["5541.20000", "1.52900000", "1534614248.456789"]]
 *   },
 *   "book-10",
 *   "XBT/USD"
 * ]
 * ```
 *
 * **Update Message:**
 * ```json
 * [
 *   1234,
 *   {
 *     "a": [["5541.30000", "0.00000000", "1534614259.123678"]],
 *     "b": [["5541.10000", "1.00000000", "1534614259.456789"]]
 *   },
 *   "book-10",
 *   "XBT/USD"
 * ]
 * ```
 *
 * @code
 * // Create Kraken connector
 * auto kraken = std::make_unique<KrakenConnector>();
 *
 * // Set depth (default is 10)
 * kraken->set_depth(25);
 *
 * // Set callbacks
 * kraken->set_orderbook_callback([](const auto& update) {
 *     std::cout << "Kraken " << update.symbol << " updated\n";
 * });
 *
 * // Connect and subscribe
 * kraken->connect();
 * kraken->subscribe("XBT/USD");  // Note: slash separator
 *
 * // Run in background
 * std::thread io_thread([&]() { kraken->run(); });
 *
 * // Cleanup
 * kraken->stop();
 * io_thread.join();
 * kraken->disconnect();
 * @endcode
 *
 * @note Symbol format uses slash separator (XBT/USD, not XBTUSD)
 * @note XBT is Kraken's symbol for Bitcoin (not BTC)
 * @see https://docs.kraken.com/websockets/
 */
class KrakenConnector : public WebSocketConnector {
public:
    /**
     * @brief Construct Kraken connector
     *
     * @param depth Order book depth (10, 25, 100, 500, 1000)
     */
    explicit KrakenConnector(uint32_t depth = 10);

    /**
     * @brief Destructor
     *
     * Automatically disconnects if still connected.
     */
    ~KrakenConnector() override;

    /**
     * @brief Connect to Kraken WebSocket API
     *
     * Establishes connection to wss://ws.kraken.com
     *
     * @return bool True if connection initiated successfully
     *
     * @note Not thread-safe
     */
    bool connect() override;

    /**
     * @brief Disconnect from Kraken
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
     * @brief Subscribe to pair's order book updates
     *
     * Subscribes to book channel for a trading pair. Receives
     * full snapshot immediately, then incremental updates.
     *
     * @param symbol Pair in Kraken format (e.g., "XBT/USD", "ETH/USD")
     * @return bool True if subscription sent successfully
     *
     * @code
     * kraken->subscribe("XBT/USD");   // Bitcoin/USD
     * kraken->subscribe("ETH/USD");   // Ethereum/USD
     * kraken->subscribe("XBT/EUR");   // Bitcoin/Euro
     * @endcode
     *
     * @note Thread-safe
     * @note Symbol must use slash separator
     * @note XBT is Kraken's symbol for Bitcoin
     * @note Snapshot is sent immediately after subscription
     */
    bool subscribe(const std::string& symbol) override;

    /**
     * @brief Unsubscribe from pair
     *
     * @param symbol Pair to unsubscribe
     * @return bool True if unsubscription sent successfully
     *
     * @note Thread-safe
     */
    bool unsubscribe(const std::string& symbol) override;

    /**
     * @brief Set order book depth
     *
     * Sets depth for future subscriptions. Must be called before subscribe().
     *
     * @param depth Depth level (10, 25, 100, 500, or 1000)
     *
     * @code
     * kraken->set_depth(100);  // Use 100 levels
     * kraken->subscribe("XBT/USD");
     * @endcode
     *
     * @warning Only affects future subscriptions
     */
    void set_depth(uint32_t depth);

    /**
     * @brief Get WebSocket URI
     *
     * @return std::string WebSocket URI for Kraken
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
     * Parses JSON message and handles subscription, snapshot, and update messages.
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
     * @brief Parse Kraken snapshot message
     *
     * Parses full order book snapshot (channelID = 0 or contains "as"/"bs").
     *
     * @param json JSON array
     * @param pair Trading pair
     * @return std::optional<NormalizedOrderBookUpdate> Parsed snapshot
     */
    std::optional<NormalizedOrderBookUpdate> parse_snapshot(
        const simdjson::dom::element& json, const std::string& pair);

    /**
     * @brief Parse Kraken update message
     *
     * Parses incremental order book update (contains "a"/"b").
     *
     * @param json JSON array
     * @param pair Trading pair
     * @return std::optional<NormalizedOrderBookUpdate> Parsed update
     */
    std::optional<NormalizedOrderBookUpdate> parse_update(
        const simdjson::dom::element& json, const std::string& pair);

    /**
     * @brief Send subscription message
     *
     * Sends subscribe/unsubscribe request to Kraken WebSocket.
     *
     * @param symbol Pair to subscribe
     * @param subscribe True to subscribe, false to unsubscribe
     */
    void send_subscription(const std::string& symbol, bool subscribe);

    /**
     * @brief Normalize Kraken symbol to standard format
     *
     * Converts Kraken's symbol format to internal format.
     *
     * @param kraken_symbol Kraken symbol (e.g., "XBT/USD")
     * @return std::string Normalized symbol
     */
    std::string normalize_symbol(const std::string& kraken_symbol) const;

    Client client_;                           ///< WebSocket client
    ConnectionHdl connection_;                ///< Connection handle
    std::string uri_;                         ///< WebSocket URI
    uint32_t depth_;                          ///< Order book depth

    simdjson::dom::parser parser_;            ///< JSON parser

    std::mutex subscriptions_mutex_;          ///< Protects subscriptions_
    std::set<std::string> subscriptions_;     ///< Subscribed pairs
    std::map<int64_t, std::string> channel_map_; ///< ChannelID -> Pair mapping

    std::atomic<bool> running_{false};        ///< Running flag
    std::atomic<uint32_t> reconnect_count_{0}; ///< Reconnection attempt counter
};

} // namespace marketdata
