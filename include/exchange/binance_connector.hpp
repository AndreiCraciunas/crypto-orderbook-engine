/**
 * @file binance_connector.hpp
 * @brief Binance WebSocket connector implementation
 *
 * Implements WebSocket connector for Binance exchange. Connects to Binance
 * WebSocket API and streams real-time order book updates.
 *
 * **Binance WebSocket API:**
 * - Base URL: wss://stream.binance.com:9443/ws
 * - Order book stream: <symbol>@depth@100ms
 * - Trade stream: <symbol>@trade
 * - Message format: JSON
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
 * @brief Binance WebSocket connector
 *
 * Connects to Binance WebSocket API for real-time market data streaming.
 * Supports order book depth updates and trade streams.
 *
 * **Binance Specifics:**
 * - Symbol format: lowercase, no separator (e.g., "btcusdt")
 * - Depth stream: Updates at 100ms intervals
 * - Snapshot required: First update is diff, need REST API for initial snapshot
 * - Rate limits: 5 requests per second, 300 connections per IP
 *
 * **Message Format:**
 * ```json
 * {
 *   "e": "depthUpdate",
 *   "E": 1234567890,
 *   "s": "BTCUSDT",
 *   "U": 157,
 *   "u": 160,
 *   "b": [["0.0024", "10"]],
 *   "a": [["0.0026", "100"]]
 * }
 * ```
 *
 * @code
 * // Create Binance connector
 * auto binance = std::make_unique<BinanceConnector>();
 *
 * // Set callback
 * binance->set_orderbook_callback([](const auto& update) {
 *     std::cout << "Binance " << update.symbol
 *               << ": " << update.bids.size() << " bids, "
 *               << update.asks.size() << " asks\n";
 * });
 *
 * // Connect and subscribe
 * binance->connect();
 * binance->subscribe("btcusdt");  // Note: lowercase
 *
 * // Run in background
 * std::thread io_thread([&]() { binance->run(); });
 *
 * // Later: cleanup
 * binance->stop();
 * io_thread.join();
 * binance->disconnect();
 * @endcode
 *
 * @note Symbol format is lowercase with no separator
 * @note First update is a diff - need REST API for snapshot
 * @see https://binance-docs.github.io/apidocs/spot/en/#websocket-market-streams
 */
class BinanceConnector : public WebSocketConnector {
public:
    /**
     * @brief Construct Binance connector
     *
     * @param use_testnet If true, connect to testnet (default: false)
     */
    explicit BinanceConnector(bool use_testnet = false);

    /**
     * @brief Destructor
     *
     * Automatically disconnects if still connected.
     */
    ~BinanceConnector() override;

    /**
     * @brief Connect to Binance WebSocket API
     *
     * Establishes connection to wss://stream.binance.com:9443/ws
     *
     * @return bool True if connection initiated successfully
     *
     * @note Not thread-safe
     */
    bool connect() override;

    /**
     * @brief Disconnect from Binance
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
     * @brief Subscribe to symbol's depth stream
     *
     * Subscribes to order book updates at 100ms intervals.
     *
     * @param symbol Symbol in Binance format (lowercase, e.g., "btcusdt")
     * @return bool True if subscription sent successfully
     *
     * @code
     * binance->subscribe("btcusdt");   // BTC/USDT
     * binance->subscribe("ethusdt");   // ETH/USDT
     * binance->subscribe("bnbbtc");    // BNB/BTC
     * @endcode
     *
     * @note Thread-safe
     * @note Symbol must be lowercase
     */
    bool subscribe(const std::string& symbol) override;

    /**
     * @brief Unsubscribe from symbol
     *
     * @param symbol Symbol to unsubscribe
     * @return bool True if unsubscription sent successfully
     *
     * @note Thread-safe
     */
    bool unsubscribe(const std::string& symbol) override;

    /**
     * @brief Get WebSocket URI
     *
     * @return std::string WebSocket URI for Binance
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
     * Parses JSON message and extracts order book updates.
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
     * @brief Parse Binance depth update message
     *
     * Parses JSON depth update and converts to NormalizedOrderBookUpdate.
     *
     * @param json JSON document
     * @return std::optional<NormalizedOrderBookUpdate> Parsed update
     */
    std::optional<NormalizedOrderBookUpdate> parse_depth_update(
        const simdjson::dom::element& json);

    /**
     * @brief Send subscription message
     *
     * Sends subscribe request to Binance WebSocket.
     *
     * @param symbol Symbol to subscribe
     * @param subscribe True to subscribe, false to unsubscribe
     */
    void send_subscription(const std::string& symbol, bool subscribe);

    Client client_;                           ///< WebSocket client
    ConnectionHdl connection_;                ///< Connection handle
    std::string uri_;                         ///< WebSocket URI
    bool use_testnet_;                        ///< Use testnet flag

    simdjson::dom::parser parser_;            ///< JSON parser

    std::mutex subscriptions_mutex_;          ///< Protects subscriptions_
    std::set<std::string> subscriptions_;     ///< Subscribed symbols

    std::atomic<bool> running_{false};        ///< Running flag
    std::atomic<uint32_t> reconnect_count_{0}; ///< Reconnection attempt counter
};

} // namespace marketdata
