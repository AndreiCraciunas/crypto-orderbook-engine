/**
 * @file frontend_rest_server.cpp
 * @brief Simple REST API server implementation for frontend
 */

#include "network/frontend_rest_server.hpp"
#include <iostream>
#include <sstream>
#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define close closesocket
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <arpa/inet.h>
#endif

namespace marketdata {

FrontendRestServer::FrontendRestServer(uint16_t port)
    : port_(port) {

#ifdef _WIN32
    // Initialize Winsock on Windows
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "FrontendRestServer: WSAStartup failed\n";
    }
#endif

    std::cout << "FrontendRestServer: Initialized on port " << port_ << "\n";
}

FrontendRestServer::~FrontendRestServer() {
    if (running_.load()) {
        stop();
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

bool FrontendRestServer::start() {
    if (running_.exchange(true)) {
        std::cerr << "FrontendRestServer: Already running\n";
        return false;
    }

    try {
        // Create socket
        server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket_ < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        // Set socket options
        int opt = 1;
#ifdef _WIN32
        setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
#else
        setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        // Bind to port
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_socket_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            throw std::runtime_error("Failed to bind to port " + std::to_string(port_));
        }

        // Listen
        if (listen(server_socket_, 10) < 0) {
            throw std::runtime_error("Failed to listen on socket");
        }

        // Start server thread
        server_thread_ = std::make_unique<std::thread>([this]() {
            server_loop();
        });

        std::cout << "FrontendRestServer: Listening on http://localhost:" << port_ << "/api/v1\n";
        return true;

    } catch (const std::exception& e) {
        std::cerr << "FrontendRestServer: Failed to start: " << e.what() << "\n";
        running_.store(false);
        if (server_socket_ >= 0) {
            close(server_socket_);
            server_socket_ = -1;
        }
        return false;
    }
}

void FrontendRestServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    std::cout << "FrontendRestServer: Stopping...\n";

    // Close server socket to unblock accept()
    if (server_socket_ >= 0) {
        close(server_socket_);
        server_socket_ = -1;
    }

    // Join server thread
    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
    }

    std::cout << "FrontendRestServer: Stopped\n";
}

void FrontendRestServer::set_orderbook_handler(HandlerFunc handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    orderbook_handler_ = handler;
}

void FrontendRestServer::set_symbols_handler(HandlerFunc handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    symbols_handler_ = handler;
}

void FrontendRestServer::server_loop() {
    std::cout << "FrontendRestServer: Server thread started\n";

    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) {
            if (running_.load()) {
                std::cerr << "FrontendRestServer: Accept error\n";
            }
            break;
        }

        // Handle request in current thread (simple blocking server)
        handle_request(client_socket);
        close(client_socket);
    }

    std::cout << "FrontendRestServer: Server thread stopped\n";
}

void FrontendRestServer::handle_request(int client_socket) {
    // Read request
    char buffer[4096];
    int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read <= 0) {
        return;
    }

    buffer[bytes_read] = '\0';
    std::string request(buffer);

    // Parse path
    std::string path = parse_request_path(request);

    // Route request
    if (path == "/api/v1/health") {
        // Health check
        std::string response = "{\"status\":\"healthy\",\"service\":\"crypto-orderbook-engine\"}";
        send_response(client_socket, 200, "application/json", response);

    } else if (path == "/api/v1/symbols") {
        // Symbols list
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        if (symbols_handler_) {
            std::string response = symbols_handler_("");
            send_response(client_socket, 200, "application/json", response);
        } else {
            std::string response = "{\"symbols\":[\"BTC-USD\",\"ETH-USD\"]}";
            send_response(client_socket, 200, "application/json", response);
        }

    } else if (path.find("/api/v1/orderbook/") == 0) {
        // Order book query
        std::string symbol = path.substr(18);  // Extract symbol after "/api/v1/orderbook/"

        std::lock_guard<std::mutex> lock(handlers_mutex_);
        if (orderbook_handler_) {
            std::string response = orderbook_handler_(symbol);
            send_response(client_socket, 200, "application/json", response);
        } else {
            std::string response = "{\"error\":\"Order book handler not configured\"}";
            send_response(client_socket, 500, "application/json", response);
        }

    } else {
        // Not found
        std::string response = "{\"error\":\"Endpoint not found\"}";
        send_response(client_socket, 404, "application/json", response);
    }
}

std::string FrontendRestServer::parse_request_path(const std::string& request) {
    // Extract path from "GET /path HTTP/1.1"
    size_t start = request.find("GET ");
    if (start == std::string::npos) {
        start = request.find("POST ");
    }
    if (start == std::string::npos) {
        return "";
    }

    start += 4;  // Skip "GET " or "POST"
    size_t end = request.find(" ", start);
    if (end == std::string::npos) {
        return "";
    }

    std::string path = request.substr(start, end - start);

    // Remove query string if present
    size_t query_pos = path.find("?");
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }

    return path;
}

void FrontendRestServer::send_response(int client_socket, int status_code,
                                       const std::string& content_type,
                                       const std::string& body) {
    std::ostringstream response;

    // Status line
    response << "HTTP/1.1 " << status_code << " ";
    switch (status_code) {
        case 200: response << "OK"; break;
        case 404: response << "Not Found"; break;
        case 500: response << "Internal Server Error"; break;
        default: response << "Unknown"; break;
    }
    response << "\r\n";

    // Headers
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << get_cors_headers();
    response << "Connection: close\r\n";
    response << "\r\n";

    // Body
    response << body;

    std::string response_str = response.str();
    send(client_socket, response_str.c_str(), response_str.length(), 0);
}

std::string FrontendRestServer::get_cors_headers() const {
    std::ostringstream headers;
    headers << "Access-Control-Allow-Origin: *\r\n";
    headers << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    headers << "Access-Control-Allow-Headers: Content-Type\r\n";
    return headers.str();
}

} // namespace marketdata
