/**
 * @file frontend_websocket_server.hpp
 * @brief WebSocket server for frontend integration using Boost.Beast
 *
 * Provides a WebSocket server at ws://localhost:8080 that broadcasts
 * market data in the format expected by the React frontend.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

namespace marketdata {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

// Forward declaration
class WebSocketSession;

/**
 * @brief WebSocket server for frontend integration
 *
 * Broadcasts JSON messages to all connected clients using Boost.Beast.
 * Runs on a background thread and handles multiple concurrent connections.
 *
 * @code
 * FrontendWebSocketServer server(8080);
 * server.start();
 *
 * // Broadcast message
 * server.broadcast("{\"type\":\"order_book_update\",\"exchange\":\"binance\",...}");
 *
 * server.stop();
 * @endcode
 *
 * @note Thread-safe
 */
class FrontendWebSocketServer {
public:

    /**
     * @brief Construct WebSocket server
     *
     * @param port Port to listen on (default: 8080)
     */
    explicit FrontendWebSocketServer(uint16_t port = 8080);

    /**
     * @brief Destructor
     *
     * Automatically stops server if running.
     */
    ~FrontendWebSocketServer();

    // Disable copy and move
    FrontendWebSocketServer(const FrontendWebSocketServer&) = delete;
    FrontendWebSocketServer& operator=(const FrontendWebSocketServer&) = delete;

    /**
     * @brief Start WebSocket server
     *
     * Begins listening for connections on ws://localhost:{port}
     *
     * @return bool True if started successfully
     */
    bool start();

    /**
     * @brief Stop WebSocket server
     *
     * Closes all connections and stops server.
     */
    void stop();

    /**
     * @brief Broadcast message to all connected clients
     *
     * @param message JSON message to broadcast
     *
     * @note Thread-safe
     * @note Non-blocking
     */
    void broadcast(const std::string& message);

    /**
     * @brief Get active connection count
     *
     * @return size_t Number of connected clients
     */
    size_t get_connection_count() const;

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

    /**
     * @brief Add session to active connections
     */
    void add_session(std::shared_ptr<WebSocketSession> session);

    /**
     * @brief Remove session from active connections
     */
    void remove_session(std::shared_ptr<WebSocketSession> session);

private:
    /**
     * @brief Server thread function
     */
    void server_thread();

    /**
     * @brief Accept new connections
     */
    void do_accept();

    uint16_t port_;                                 ///< Server port
    std::atomic<bool> running_{false};              ///< Running flag

    std::unique_ptr<net::io_context> ioc_;          ///< ASIO io_context
    std::unique_ptr<tcp::acceptor> acceptor_;       ///< TCP acceptor
    std::unique_ptr<std::thread> server_thread_;    ///< Server thread

    std::set<std::shared_ptr<WebSocketSession>> sessions_; ///< Active sessions
    mutable std::mutex sessions_mutex_;             ///< Protects sessions set

    size_t message_count_{0};                       ///< Broadcast message count
};

/**
 * @brief WebSocket session - one per client connection
 */
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    explicit WebSocketSession(tcp::socket socket, FrontendWebSocketServer* server);
    ~WebSocketSession();

    void run();
    void send(const std::string& message);
    void close();

private:
    void on_accept(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);

    websocket::stream<tcp::socket> ws_;
    FrontendWebSocketServer* server_;
    beast::flat_buffer buffer_;
    std::vector<std::string> write_queue_;
    std::mutex write_mutex_;
    bool writing_{false};
};

} // namespace marketdata
