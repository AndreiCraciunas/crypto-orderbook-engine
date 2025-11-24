/**
 * @file frontend_rest_server.hpp
 * @brief Simple REST API server for frontend integration
 *
 * Provides REST API endpoints at http://localhost:8081/api/v1
 * for querying current market data state.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <string>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

namespace marketdata {

/**
 * @brief Simple HTTP REST API server for frontend
 *
 * Provides basic REST endpoints for market data queries.
 * Uses a minimal HTTP server implementation.
 *
 * Endpoints:
 * - GET /api/v1/health - Health check
 * - GET /api/v1/symbols - List available symbols
 * - GET /api/v1/orderbook/{symbol} - Get order book snapshot
 *
 * @code
 * FrontendRestServer server(8081);
 *
 * // Register handlers
 * server.set_orderbook_handler([](const std::string& symbol) -> std::string {
 *     return "{\"symbol\":\"" + symbol + "\",\"bids\":[],\"asks\":[]}";
 * });
 *
 * server.start();
 * @endcode
 */
class FrontendRestServer {
public:
    using HandlerFunc = std::function<std::string(const std::string&)>;

    /**
     * @brief Construct REST API server
     *
     * @param port Port to listen on (default: 8081)
     */
    explicit FrontendRestServer(uint16_t port = 8081);

    /**
     * @brief Destructor
     */
    ~FrontendRestServer();

    // Disable copy and move
    FrontendRestServer(const FrontendRestServer&) = delete;
    FrontendRestServer& operator=(const FrontendRestServer&) = delete;

    /**
     * @brief Start REST API server
     *
     * @return bool True if started successfully
     */
    bool start();

    /**
     * @brief Stop REST API server
     */
    void stop();

    /**
     * @brief Set order book query handler
     *
     * @param handler Function that returns order book JSON for a symbol
     */
    void set_orderbook_handler(HandlerFunc handler);

    /**
     * @brief Set symbols list handler
     *
     * @param handler Function that returns list of available symbols
     */
    void set_symbols_handler(HandlerFunc handler);

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
    std::atomic<bool> running_{false};              ///< Running flag

    std::unique_ptr<std::thread> server_thread_;    ///< Server thread
    int server_socket_{-1};                         ///< Server socket

    mutable std::mutex handlers_mutex_;             ///< Protects handlers
    HandlerFunc orderbook_handler_;                 ///< Order book query handler
    HandlerFunc symbols_handler_;                   ///< Symbols list handler

    /**
     * @brief Server main loop
     */
    void server_loop();

    /**
     * @brief Handle HTTP request
     */
    void handle_request(int client_socket);

    /**
     * @brief Parse HTTP request path
     */
    std::string parse_request_path(const std::string& request);

    /**
     * @brief Send HTTP response
     */
    void send_response(int client_socket, int status_code,
                      const std::string& content_type,
                      const std::string& body);

    /**
     * @brief Send CORS headers
     */
    std::string get_cors_headers() const;
};

} // namespace marketdata
