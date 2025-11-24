/**
 * @file rest_api_server.cpp
 * @brief REST API server implementation
 *
 * NOTE: This is a simplified implementation for demonstration.
 * For production use, consider using Boost.Beast, cpp-httplib, or similar library.
 */

#include "network/rest_api_server.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

namespace marketdata {

// Helper function to get exchange name
const char* exchange_name_str(Exchange ex) {
    switch (ex) {
        case Exchange::BINANCE: return "Binance";
        case Exchange::COINBASE: return "Coinbase";
        case Exchange::KRAKEN: return "Kraken";
        default: return "Unknown";
    }
}

RestAPIServer::RestAPIServer(uint16_t port, const std::string& host)
    : port_(port)
    , host_(host) {

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    spdlog::info("RestAPIServer: Created on {}:{}", host_, port_);
}

RestAPIServer::~RestAPIServer() {
    if (running_.load()) {
        stop();
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

bool RestAPIServer::start() {
    if (running_.exchange(true)) {
        spdlog::warn("RestAPIServer: Already running");
        return false;
    }

    start_time_ns_ = get_timestamp_ns();

    // Start server thread
    server_thread_ = std::make_unique<std::thread>([this]() {
        server_loop();
    });

    spdlog::info("RestAPIServer: Started on {}:{}", host_, port_);
    return true;
}

void RestAPIServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    spdlog::info("RestAPIServer: Stopping");

    // Close server socket
    if (server_socket_ != INVALID_SOCKET) {
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
    }

    // Join thread
    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
    }

    spdlog::info("RestAPIServer: Stopped");
}

void RestAPIServer::register_orderbook(const std::string& symbol,
                                      std::shared_ptr<AggregatedOrderBook> orderbook) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    orderbooks_[symbol] = orderbook;
    spdlog::info("RestAPIServer: Registered orderbook for {}", symbol);
}

void RestAPIServer::register_analytics(const std::string& symbol,
                                      std::shared_ptr<MarketAnalytics> analytics) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    analytics_[symbol] = analytics;
    spdlog::info("RestAPIServer: Registered analytics for {}", symbol);
}

void RestAPIServer::register_arbitrage_monitor(std::shared_ptr<ArbitrageMonitor> monitor) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    arbitrage_monitor_ = monitor;
    spdlog::info("RestAPIServer: Registered arbitrage monitor");
}

void RestAPIServer::server_loop() {
    // Create socket
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ == INVALID_SOCKET) {
        spdlog::error("RestAPIServer: Failed to create socket");
        running_.store(false);
        return;
    }

    // Set socket options
    int opt = 1;
#ifdef _WIN32
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    // Bind socket
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_socket_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        spdlog::error("RestAPIServer: Failed to bind socket");
        running_.store(false);
        return;
    }

    // Listen
    if (listen(server_socket_, 10) < 0) {
        spdlog::error("RestAPIServer: Failed to listen");
        running_.store(false);
        return;
    }

    spdlog::info("RestAPIServer: Listening on port {}", port_);

    // Accept connections (simplified for demonstration)
    char buffer[4096];
    while (running_.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        SOCKET client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket == INVALID_SOCKET) {
            continue;
        }

        // Read request
        int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            std::string request(buffer);

            // Handle request
            std::string response = handle_request(request);

            // Send response
            send(client_socket, response.c_str(), response.length(), 0);
        }

        closesocket(client_socket);
    }
}

std::string RestAPIServer::handle_request(const std::string& request) {
    // Parse HTTP request (simplified)
    std::istringstream iss(request);
    std::string method, url, version;
    iss >> method >> url >> version;

    if (method != "GET") {
        return create_error_response(405, "Method Not Allowed");
    }

    // Parse URL
    std::string path, query;
    parse_url(url, path, query);

    // Route request
    if (path == "/api/v1/health") {
        return create_http_response(200, handle_health());
    } else if (path == "/api/v1/status") {
        return create_http_response(200, handle_status());
    } else if (path == "/api/v1/arbitrage") {
        return create_http_response(200, handle_arbitrage());
    } else if (path.find("/api/v1/orderbook/") == 0) {
        std::string symbol = path.substr(18);  // After "/api/v1/orderbook/"
        return create_http_response(200, handle_orderbook(symbol, query));
    } else if (path.find("/api/v1/analytics/") == 0) {
        std::string symbol = path.substr(18);  // After "/api/v1/analytics/"
        return create_http_response(200, handle_analytics(symbol));
    } else {
        return create_error_response(404, "Not Found");
    }
}

std::string RestAPIServer::handle_orderbook(const std::string& symbol, const std::string& query) {
    std::lock_guard<std::mutex> lock(data_mutex_);

    auto it = orderbooks_.find(symbol);
    if (it == orderbooks_.end()) {
        return "{\"error\":\"Symbol not found\"}";
    }

    // Get depth from query (default: 10)
    size_t depth = 10;
    // Simple query parsing (for demonstration)

    auto snapshot = it->second->get_snapshot(depth);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8);

    oss << "{\"symbol\":\"" << symbol << "\","
        << "\"timestamp\":" << snapshot.timestamp_ns << ",";

    // Mid price and spread
    if (auto mid = snapshot.get_mid_price()) {
        oss << "\"mid_price\":" << *mid << ",";
    }
    if (auto spread = snapshot.get_spread()) {
        oss << "\"spread\":" << *spread << ",";
    }

    // Bids
    oss << "\"bids\":[";
    for (size_t i = 0; i < snapshot.bids.size(); i++) {
        if (i > 0) oss << ",";
        const auto& bid = snapshot.bids[i];
        oss << "[" << bid.price << "," << bid.quantity << ",\""
            << exchange_name_str(bid.exchange) << "\"]";
    }
    oss << "],";

    // Asks
    oss << "\"asks\":[";
    for (size_t i = 0; i < snapshot.asks.size(); i++) {
        if (i > 0) oss << ",";
        const auto& ask = snapshot.asks[i];
        oss << "[" << ask.price << "," << ask.quantity << ",\""
            << exchange_name_str(ask.exchange) << "\"]";
    }
    oss << "]}";

    return oss.str();
}

std::string RestAPIServer::handle_analytics(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(data_mutex_);

    auto it = analytics_.find(symbol);
    if (it == analytics_.end()) {
        return "{\"error\":\"Symbol not found\"}";
    }

    auto& analytics = it->second;
    auto spread_stats = analytics->get_spread_stats();

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    oss << "{\"symbol\":\"" << symbol << "\","
        << "\"vwap\":" << analytics->get_vwap() << ","
        << "\"twap\":" << analytics->get_twap() << ","
        << "\"imbalance\":" << std::setprecision(4) << analytics->get_imbalance() << ","
        << "\"spread\":{"
        << "\"current\":" << std::setprecision(2) << spread_stats.current_spread << ","
        << "\"average\":" << spread_stats.average_spread << ","
        << "\"min\":" << spread_stats.min_spread << ","
        << "\"max\":" << spread_stats.max_spread
        << "}}";

    return oss.str();
}

std::string RestAPIServer::handle_arbitrage() {
    std::lock_guard<std::mutex> lock(data_mutex_);

    if (!arbitrage_monitor_) {
        return "{\"error\":\"Arbitrage monitor not registered\"}";
    }

    auto opportunities = arbitrage_monitor_->get_active_opportunities();

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    oss << "{\"opportunities\":[";
    for (size_t i = 0; i < opportunities.size(); i++) {
        if (i > 0) oss << ",";
        const auto& opp = opportunities[i].opportunity;
        oss << "{"
            << "\"symbol\":\"" << opp.symbol << "\","
            << "\"buy_exchange\":\"" << exchange_name_str(opp.buy_exchange) << "\","
            << "\"sell_exchange\":\"" << exchange_name_str(opp.sell_exchange) << "\","
            << "\"profit_percent\":" << opp.profit_percentage
            << "}";
    }
    oss << "]}";

    return oss.str();
}

std::string RestAPIServer::handle_health() {
    uint64_t uptime_ns = get_timestamp_ns() - start_time_ns_;
    uint64_t uptime_sec = uptime_ns / 1'000'000'000;

    std::ostringstream oss;
    oss << "{\"status\":\"healthy\","
        << "\"uptime_seconds\":" << uptime_sec
        << "}";

    return oss.str();
}

std::string RestAPIServer::handle_status() {
    std::lock_guard<std::mutex> lock(data_mutex_);

    std::ostringstream oss;
    oss << "{\"orderbooks\":" << orderbooks_.size() << ","
        << "\"analytics\":" << analytics_.size() << ","
        << "\"arbitrage_enabled\":" << (arbitrage_monitor_ ? "true" : "false")
        << "}";

    return oss.str();
}

std::string RestAPIServer::create_http_response(int status_code, const std::string& body,
                                                const std::string& content_type) {
    std::ostringstream oss;

    std::string status_text = (status_code == 200) ? "OK" : "Error";

    oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.length() << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << body;

    return oss.str();
}

std::string RestAPIServer::create_error_response(int status_code, const std::string& message) {
    std::ostringstream oss;
    oss << "{\"error\":\"" << message << "\",\"code\":" << status_code << "}";
    return create_http_response(status_code, oss.str());
}

void RestAPIServer::parse_url(const std::string& url, std::string& path, std::string& query) {
    size_t query_pos = url.find('?');
    if (query_pos != std::string::npos) {
        path = url.substr(0, query_pos);
        query = url.substr(query_pos + 1);
    } else {
        path = url;
        query = "";
    }
}

} // namespace marketdata
