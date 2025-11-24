/**
 * @file rest_api_server.hpp
 * @brief REST API server for querying market data
 *
 * Provides HTTP REST API for querying order books, analytics, and system status.
 * Designed for integration with external systems and monitoring tools.
 *
 * **Endpoints:**
 * - GET /api/v1/orderbook/{symbol} - Get aggregated order book
 * - GET /api/v1/analytics/{symbol} - Get analytics for symbol
 * - GET /api/v1/arbitrage - Get current arbitrage opportunities
 * - GET /api/v1/health - Health check
 * - GET /api/v1/status - System status
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "core/types.hpp"
#include "core/aggregated_order_book.hpp"
#include "analytics/market_analytics.hpp"
#include "trading/arbitrage_monitor.hpp"
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

namespace marketdata {

/**
 * @brief REST API server
 *
 * Simple HTTP server providing REST API for market data queries.
 *
 * **API Documentation:**
 *
 * **GET /api/v1/orderbook/{symbol}?depth=10**
 * Returns aggregated order book for symbol.
 *
 * Response:
 * ```json
 * {
 *   "symbol": "BTCUSD",
 *   "timestamp": 1234567890,
 *   "bids": [[42000.0, 1.5, "Binance"], ...],
 *   "asks": [[42001.0, 2.0, "Coinbase"], ...],
 *   "mid_price": 42000.5,
 *   "spread": 1.0
 * }
 * ```
 *
 * **GET /api/v1/analytics/{symbol}**
 * Returns analytics for symbol.
 *
 * Response:
 * ```json
 * {
 *   "symbol": "BTCUSD",
 *   "vwap": 42050.0,
 *   "twap": 42048.0,
 *   "imbalance": 0.15,
 *   "spread": {
 *     "current": 1.0,
 *     "average": 1.2,
 *     "min": 0.5,
 *     "max": 2.5
 *   }
 * }
 * ```
 *
 * **GET /api/v1/arbitrage**
 * Returns current arbitrage opportunities.
 *
 * **GET /api/v1/health**
 * Health check endpoint.
 *
 * Response:
 * ```json
 * {
 *   "status": "healthy",
 *   "uptime_seconds": 3600,
 *   "active_connections": 42
 * }
 * ```
 *
 * @code
 * // Create API server
 * RestAPIServer api(8081);
 *
 * // Register data sources
 * api.register_orderbook("BTCUSD", agg_book);
 * api.register_analytics("BTCUSD", analytics);
 * api.register_arbitrage_monitor(arb_monitor);
 *
 * // Start server
 * api.start();
 * @endcode
 *
 * @note Thread-safe
 * @note Uses simple built-in HTTP server (no external dependencies)
 */
class RestAPIServer {
public:
    /**
     * @brief Construct REST API server
     *
     * @param port Port to listen on
     * @param host Host to bind to (default: 0.0.0.0)
     */
    explicit RestAPIServer(uint16_t port, const std::string& host = "0.0.0.0");

    /**
     * @brief Destructor
     */
    ~RestAPIServer();

    // Disable copy and move
    RestAPIServer(const RestAPIServer&) = delete;
    RestAPIServer& operator=(const RestAPIServer&) = delete;

    /**
     * @brief Start API server
     *
     * @return bool True if started successfully
     */
    bool start();

    /**
     * @brief Stop API server
     */
    void stop();

    /**
     * @brief Register aggregated order book
     *
     * @param symbol Trading symbol
     * @param orderbook Aggregated order book pointer
     */
    void register_orderbook(const std::string& symbol,
                           std::shared_ptr<AggregatedOrderBook> orderbook);

    /**
     * @brief Register market analytics
     *
     * @param symbol Trading symbol
     * @param analytics Market analytics pointer
     */
    void register_analytics(const std::string& symbol,
                           std::shared_ptr<MarketAnalytics> analytics);

    /**
     * @brief Register arbitrage monitor
     *
     * @param monitor Arbitrage monitor pointer
     */
    void register_arbitrage_monitor(std::shared_ptr<ArbitrageMonitor> monitor);

    /**
     * @brief Check if server is running
     *
     * @return bool True if running
     */
    bool is_running() const { return running_.load(); }

    /**
     * @brief Get server port
     *
     * @return uint16_t Server port
     */
    uint16_t get_port() const { return port_; }

private:
    uint16_t port_;                                 ///< Server port
    std::string host_;                              ///< Bind host
    std::atomic<bool> running_{false};              ///< Running flag
    uint64_t start_time_ns_{0};                     ///< Start timestamp

    std::unique_ptr<std::thread> server_thread_;    ///< Server thread
    int server_socket_{-1};                         ///< Server socket

    mutable std::mutex data_mutex_;                 ///< Protects data sources
    std::map<std::string, std::shared_ptr<AggregatedOrderBook>> orderbooks_;
    std::map<std::string, std::shared_ptr<MarketAnalytics>> analytics_;
    std::shared_ptr<ArbitrageMonitor> arbitrage_monitor_;

    /**
     * @brief Server main loop
     */
    void server_loop();

    /**
     * @brief Handle HTTP request
     */
    std::string handle_request(const std::string& request);

    /**
     * @brief Handle GET /api/v1/orderbook/{symbol}
     */
    std::string handle_orderbook(const std::string& symbol, const std::string& query);

    /**
     * @brief Handle GET /api/v1/analytics/{symbol}
     */
    std::string handle_analytics(const std::string& symbol);

    /**
     * @brief Handle GET /api/v1/arbitrage
     */
    std::string handle_arbitrage();

    /**
     * @brief Handle GET /api/v1/health
     */
    std::string handle_health();

    /**
     * @brief Handle GET /api/v1/status
     */
    std::string handle_status();

    /**
     * @brief Create HTTP response
     */
    std::string create_http_response(int status_code, const std::string& body,
                                    const std::string& content_type = "application/json");

    /**
     * @brief Create error response
     */
    std::string create_error_response(int status_code, const std::string& message);

    /**
     * @brief Parse URL path and query
     */
    void parse_url(const std::string& url, std::string& path, std::string& query);
};

} // namespace marketdata
