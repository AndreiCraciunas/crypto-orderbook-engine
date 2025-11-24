/**
 * @file frontend_websocket_server.cpp
 * @brief WebSocket server implementation using Boost.Beast
 */

#include "network/frontend_websocket_server.hpp"
#include <iostream>
#include <chrono>

namespace marketdata {

// ============================================================================
// WebSocketSession Implementation
// ============================================================================

WebSocketSession::WebSocketSession(tcp::socket socket, FrontendWebSocketServer* server)
    : ws_(std::move(socket))
    , server_(server) {
}

WebSocketSession::~WebSocketSession() {
    if (server_) {
        server_->remove_session(shared_from_this());
    }
}

void WebSocketSession::run() {
    // Set suggested timeout settings for the websocket
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    // Set a decorator to change the Server of the handshake
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(http::field::server, "Crypto-OrderBook-Engine");
        }));

    // Accept the websocket handshake
    ws_.async_accept(
        beast::bind_front_handler(
            &WebSocketSession::on_accept,
            shared_from_this()));
}

void WebSocketSession::on_accept(beast::error_code ec) {
    if (ec) {
        std::cerr << "WebSocket accept error: " << ec.message() << "\n";
        return;
    }

    // Send welcome message
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    std::string welcome = R"({"type":"connected","message":"Connected to Crypto Order Book Engine","timestamp":)"
                        + std::to_string(timestamp) + "}";

    ws_.text(true);
    ws_.async_write(
        net::buffer(welcome),
        [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (!ec) {
                self->do_read();
            }
        });
}

void WebSocketSession::do_read() {
    // Read a message into our buffer
    ws_.async_read(
        buffer_,
        beast::bind_front_handler(
            &WebSocketSession::on_read,
            shared_from_this()));
}

void WebSocketSession::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    // Handle connection close
    if (ec == websocket::error::closed) {
        return;
    }

    if (ec) {
        std::cerr << "WebSocket read error: " << ec.message() << "\n";
        return;
    }

    // Handle ping/pong
    std::string message = beast::buffers_to_string(buffer_.data());
    if (message.find("\"type\":\"ping\"") != std::string::npos) {
        buffer_.consume(buffer_.size());
        std::string pong = R"({"type":"pong"})";
        ws_.text(true);
        ws_.async_write(
            net::buffer(pong),
            [self = shared_from_this()](beast::error_code, std::size_t) {
                self->do_read();
            });
        return;
    }

    // Clear the buffer
    buffer_.consume(buffer_.size());

    // Continue reading
    do_read();
}

void WebSocketSession::send(const std::string& message) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    write_queue_.push_back(message);

    if (write_queue_.size() == 1) {
        ws_.text(true);
        ws_.async_write(
            net::buffer(write_queue_.front()),
            beast::bind_front_handler(
                &WebSocketSession::on_write,
                shared_from_this()));
    }
}

void WebSocketSession::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        std::cerr << "WebSocket write error: " << ec.message() << "\n";
        return;
    }

    std::lock_guard<std::mutex> lock(write_mutex_);

    write_queue_.erase(write_queue_.begin());

    if (!write_queue_.empty()) {
        ws_.text(true);
        ws_.async_write(
            net::buffer(write_queue_.front()),
            beast::bind_front_handler(
                &WebSocketSession::on_write,
                shared_from_this()));
    }
}

void WebSocketSession::close() {
    beast::error_code ec;
    ws_.close(websocket::close_code::normal, ec);
}

// ============================================================================
// FrontendWebSocketServer Implementation
// ============================================================================

FrontendWebSocketServer::FrontendWebSocketServer(uint16_t port)
    : port_(port) {
    std::cout << "FrontendWebSocketServer: Initialized on port " << port_ << "\n";
}

FrontendWebSocketServer::~FrontendWebSocketServer() {
    if (running_.load()) {
        stop();
    }
}

bool FrontendWebSocketServer::start() {
    if (running_.exchange(true)) {
        std::cerr << "FrontendWebSocketServer: Already running\n";
        return false;
    }

    try {
        // Start server thread
        server_thread_ = std::make_unique<std::thread>([this]() { this->server_thread(); });

        std::cout << "FrontendWebSocketServer: Started on ws://localhost:" << port_ << "\n";
        return true;

    } catch (const std::exception& e) {
        std::cerr << "FrontendWebSocketServer: Failed to start - " << e.what() << "\n";
        running_.store(false);
        return false;
    }
}

void FrontendWebSocketServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    try {
        // Close all sessions
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (auto& session : sessions_) {
                session->close();
            }
            sessions_.clear();
        }

        // Stop io_context
        if (ioc_) {
            ioc_->stop();
        }

        // Wait for thread to finish
        if (server_thread_ && server_thread_->joinable()) {
            server_thread_->join();
        }

        std::cout << "FrontendWebSocketServer: Stopped\n";

    } catch (const std::exception& e) {
        std::cerr << "FrontendWebSocketServer: Error during stop - " << e.what() << "\n";
    }
}

void FrontendWebSocketServer::broadcast(const std::string& message) {
    if (!running_.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(sessions_mutex_);

    // Log first broadcast and then every 100th message to avoid spam
    message_count_++;
    if (message_count_ == 1 || message_count_ % 100 == 0) {
        std::cout << "[WS Broadcast #" << message_count_ << " to "
                  << sessions_.size() << " clients] "
                  << message.substr(0, 100)
                  << (message.length() > 100 ? "..." : "") << "\n";
    }

    // Send to all connected sessions
    for (auto& session : sessions_) {
        session->send(message);
    }
}

size_t FrontendWebSocketServer::get_connection_count() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.size();
}

void FrontendWebSocketServer::add_session(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.insert(session);
    std::cout << "WebSocket client connected (total: " << sessions_.size() << ")\n";
}

void FrontendWebSocketServer::remove_session(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session);
    std::cout << "WebSocket client disconnected (remaining: " << sessions_.size() << ")\n";
}

void FrontendWebSocketServer::server_thread() {
    try {
        // Create io_context
        ioc_ = std::make_unique<net::io_context>(1);

        // Create acceptor
        acceptor_ = std::make_unique<tcp::acceptor>(*ioc_);

        tcp::endpoint endpoint{tcp::v4(), port_};
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(net::socket_base::reuse_address(true));
        acceptor_->bind(endpoint);
        acceptor_->listen(net::socket_base::max_listen_connections);

        std::cout << "WebSocket server listening on port " << port_ << "\n";

        // Start accepting connections
        do_accept();

        // Run the I/O service
        ioc_->run();

        std::cout << "WebSocket server thread stopped\n";

    } catch (const std::exception& e) {
        std::cerr << "WebSocket server thread error: " << e.what() << "\n";
        running_.store(false);
    }
}

void FrontendWebSocketServer::do_accept() {
    if (!running_.load() || !acceptor_) {
        return;
    }

    acceptor_->async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                // Create session and add to active sessions
                auto session = std::make_shared<WebSocketSession>(std::move(socket), this);
                add_session(session);
                session->run();
            } else {
                std::cerr << "Accept error: " << ec.message() << "\n";
            }

            // Continue accepting
            do_accept();
        });
}

} // namespace marketdata
