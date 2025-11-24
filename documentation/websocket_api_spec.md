# WebSocket Server and REST API Specification

## Overview
Implement a high-performance WebSocket server for streaming market data to frontend clients and a REST API for querying current market state, with sub-millisecond latency and support for thousands of concurrent connections.

## WebSocket Server Architecture

### Core Server Implementation
```cpp
class MarketDataWebSocketServer {
private:
    // Server configuration
    struct Config {
        uint16_t port = 8080;
        size_t max_connections = 10000;
        size_t max_message_size = 65536;
        std::chrono::milliseconds ping_interval{30000};
        std::chrono::milliseconds connection_timeout{60000};
        bool use_compression = true;
        bool use_binary_protocol = true;
    };
    
    // WebSocket server with TLS support
    websocketpp::server<websocketpp::config::asio_tls> wss_server;
    websocketpp::server<websocketpp::config::asio> ws_server;
    
    // Client management
    struct ClientSession {
        websocketpp::connection_hdl connection;
        std::string client_id;
        std::unordered_set<std::string> subscriptions;
        std::atomic<uint64_t> last_activity{0};
        std::atomic<uint64_t> messages_sent{0};
        std::atomic<uint64_t> bytes_sent{0};
        bool use_binary = true;
        int compression_level = 1;
    };
    
    // Thread-safe client registry
    mutable std::shared_mutex clients_mutex;
    std::unordered_map<std::string, ClientSession> clients;
    
    // Message broadcasting
    LockFreeQueue<BroadcastMessage> broadcast_queue;
    std::thread broadcast_thread;
    
    // Performance monitoring
    std::atomic<uint64_t> total_connections{0};
    std::atomic<uint64_t> active_connections{0};
    std::atomic<uint64_t> messages_broadcasted{0};
    
public:
    void start(const Config& config) {
        // Configure server
        configure_server(config);
        
        // Start broadcast thread
        broadcast_thread = std::thread([this]() {
            process_broadcast_queue();
        });
        
        // Start WebSocket server
        wss_server.listen(config.port);
        wss_server.start_accept();
        
        // Run server (blocking)
        wss_server.run();
    }
    
    void broadcast_order_book_update(const OrderBookUpdate& update) {
        BroadcastMessage msg;
        msg.type = MessageType::ORDER_BOOK_UPDATE;
        msg.symbol = update.symbol;
        msg.data = serialize_update(update);
        
        broadcast_queue.push(std::move(msg));
    }
};
```

### Binary Protocol Definition
```cpp
// Protocol Buffer definition (market_data.proto)
syntax = "proto3";

package marketdata;

message OrderBookUpdate {
    enum Exchange {
        UNKNOWN = 0;
        BINANCE = 1;
        COINBASE = 2;
        KRAKEN = 3;
    }
    
    message PriceLevel {
        double price = 1;
        double quantity = 2;
        uint32 order_count = 3;
    }
    
    Exchange exchange = 1;
    string symbol = 2;
    uint64 timestamp_ns = 3;
    uint64 sequence_number = 4;
    
    repeated PriceLevel bids = 5;
    repeated PriceLevel asks = 6;
}

message Trade {
    enum Side {
        UNKNOWN_SIDE = 0;
        BUY = 1;
        SELL = 2;
    }
    
    string exchange = 1;
    string symbol = 2;
    uint64 timestamp_ns = 3;
    string trade_id = 4;
    double price = 5;
    double quantity = 6;
    Side side = 7;
}

message MarketDataMessage {
    oneof message {
        OrderBookUpdate order_book_update = 1;
        Trade trade = 2;
        Statistics statistics = 3;
        Heartbeat heartbeat = 4;
    }
}
```

### Message Serialization
```cpp
class MessageSerializer {
private:
    // Pre-allocated buffers for zero-copy serialization
    thread_local std::array<uint8_t, 65536> buffer;
    
public:
    // Binary serialization (Protocol Buffers)
    std::string_view serialize_binary(const OrderBookUpdate& update) {
        marketdata::OrderBookUpdate proto_msg;
        
        proto_msg.set_exchange(static_cast<marketdata::OrderBookUpdate::Exchange>(update.exchange));
        proto_msg.set_symbol(update.symbol);
        proto_msg.set_timestamp_ns(update.timestamp);
        proto_msg.set_sequence_number(update.sequence);
        
        // Add bids
        for (const auto& bid : update.bids) {
            auto* level = proto_msg.add_bids();
            level->set_price(bid.price);
            level->set_quantity(bid.quantity);
            level->set_order_count(bid.order_count);
        }
        
        // Add asks
        for (const auto& ask : update.asks) {
            auto* level = proto_msg.add_asks();
            level->set_price(ask.price);
            level->set_quantity(ask.quantity);
            level->set_order_count(ask.order_count);
        }
        
        size_t size = proto_msg.ByteSizeLong();
        proto_msg.SerializeToArray(buffer.data(), size);
        
        return std::string_view(reinterpret_cast<char*>(buffer.data()), size);
    }
    
    // JSON serialization (fallback for web clients)
    std::string serialize_json(const OrderBookUpdate& update) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        
        writer.StartObject();
        writer.Key("type");
        writer.String("order_book_update");
        writer.Key("exchange");
        writer.String(exchange_to_string(update.exchange));
        writer.Key("symbol");
        writer.String(update.symbol);
        writer.Key("timestamp");
        writer.Uint64(update.timestamp);
        writer.Key("sequence");
        writer.Uint64(update.sequence);
        
        // Serialize bids
        writer.Key("bids");
        writer.StartArray();
        for (const auto& bid : update.bids) {
            writer.StartArray();
            writer.Double(bid.price);
            writer.Double(bid.quantity);
            writer.EndArray();
        }
        writer.EndArray();
        
        // Serialize asks
        writer.Key("asks");
        writer.StartArray();
        for (const auto& ask : update.asks) {
            writer.StartArray();
            writer.Double(ask.price);
            writer.Double(ask.quantity);
            writer.EndArray();
        }
        writer.EndArray();
        
        writer.EndObject();
        
        return buffer.GetString();
    }
};
```

### Client Connection Management
```cpp
class ConnectionManager {
private:
    struct ConnectionPool {
        std::vector<std::unique_ptr<ClientSession>> pool;
        std::mutex mutex;
        
        ClientSession* acquire() {
            std::lock_guard<std::mutex> lock(mutex);
            if (!pool.empty()) {
                auto session = std::move(pool.back());
                pool.pop_back();
                return session.release();
            }
            return new ClientSession();
        }
        
        void release(ClientSession* session) {
            session->reset();
            std::lock_guard<std::mutex> lock(mutex);
            pool.emplace_back(session);
        }
    };
    
    ConnectionPool session_pool;
    
public:
    void handle_new_connection(websocketpp::connection_hdl hdl) {
        auto* session = session_pool.acquire();
        session->connection = hdl;
        session->client_id = generate_client_id();
        session->last_activity = get_timestamp_ns();
        
        // Send initial snapshot
        send_initial_snapshot(session);
        
        // Add to active sessions
        {
            std::unique_lock<std::shared_mutex> lock(clients_mutex);
            clients[session->client_id] = *session;
        }
        
        active_connections.fetch_add(1);
        total_connections.fetch_add(1);
    }
    
    void handle_disconnect(websocketpp::connection_hdl hdl) {
        std::unique_lock<std::shared_mutex> lock(clients_mutex);
        
        auto it = std::find_if(clients.begin(), clients.end(),
            [&hdl](const auto& pair) {
                return pair.second.connection.lock() == hdl.lock();
            });
        
        if (it != clients.end()) {
            session_pool.release(&it->second);
            clients.erase(it);
            active_connections.fetch_sub(1);
        }
    }
};
```

### Subscription Management
```cpp
class SubscriptionManager {
private:
    // Symbol to clients mapping for efficient broadcasting
    struct SymbolSubscribers {
        mutable std::shared_mutex mutex;
        std::unordered_set<std::string> client_ids;
    };
    
    std::unordered_map<std::string, SymbolSubscribers> symbol_subscribers;
    
public:
    void subscribe(const std::string& client_id, const std::string& symbol) {
        auto& subscribers = symbol_subscribers[symbol];
        std::unique_lock<std::shared_mutex> lock(subscribers.mutex);
        subscribers.client_ids.insert(client_id);
    }
    
    void unsubscribe(const std::string& client_id, const std::string& symbol) {
        auto it = symbol_subscribers.find(symbol);
        if (it != symbol_subscribers.end()) {
            std::unique_lock<std::shared_mutex> lock(it->second.mutex);
            it->second.client_ids.erase(client_id);
        }
    }
    
    std::vector<std::string> get_subscribers(const std::string& symbol) const {
        auto it = symbol_subscribers.find(symbol);
        if (it != symbol_subscribers.end()) {
            std::shared_lock<std::shared_mutex> lock(it->second.mutex);
            return std::vector<std::string>(
                it->second.client_ids.begin(),
                it->second.client_ids.end()
            );
        }
        return {};
    }
};
```

## REST API Implementation

### HTTP Server
```cpp
class MarketDataRestAPI {
private:
    httplib::SSLServer server;
    OrderBookManager& order_book_manager;
    MetricsCollector& metrics;
    
public:
    void setup_routes() {
        // Get order book snapshot
        server.Get("/api/v1/orderbook/:symbol", 
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_orderbook_request(req, res);
            });
        
        // Get aggregated order book
        server.Get("/api/v1/orderbook/aggregated/:symbol",
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_aggregated_orderbook_request(req, res);
            });
        
        // Get recent trades
        server.Get("/api/v1/trades/:symbol",
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_trades_request(req, res);
            });
        
        // Get market statistics
        server.Get("/api/v1/stats/:symbol",
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_stats_request(req, res);
            });
        
        // Get system metrics
        server.Get("/api/v1/metrics",
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_metrics_request(req, res);
            });
        
        // Health check
        server.Get("/health",
            [this](const httplib::Request& req, httplib::Response& res) {
                res.set_content("{\"status\":\"healthy\"}", "application/json");
            });
    }
    
    void handle_orderbook_request(const httplib::Request& req, httplib::Response& res) {
        auto symbol = req.path_params.at("symbol");
        auto depth = req.has_param("depth") ? 
            std::stoi(req.get_param_value("depth")) : 20;
        
        auto snapshot = order_book_manager.get_snapshot(symbol, depth);
        
        if (snapshot) {
            rapidjson::Document doc;
            doc.SetObject();
            auto& allocator = doc.GetAllocator();
            
            doc.AddMember("symbol", rapidjson::Value(symbol.c_str(), allocator), allocator);
            doc.AddMember("timestamp", snapshot->timestamp, allocator);
            
            // Add bids
            rapidjson::Value bids(rapidjson::kArrayType);
            for (const auto& bid : snapshot->bids) {
                rapidjson::Value level(rapidjson::kArrayType);
                level.PushBack(bid.price, allocator);
                level.PushBack(bid.quantity, allocator);
                bids.PushBack(level, allocator);
            }
            doc.AddMember("bids", bids, allocator);
            
            // Add asks
            rapidjson::Value asks(rapidjson::kArrayType);
            for (const auto& ask : snapshot->asks) {
                rapidjson::Value level(rapidjson::kArrayType);
                level.PushBack(ask.price, allocator);
                level.PushBack(ask.quantity, allocator);
                asks.PushBack(level, allocator);
            }
            doc.AddMember("asks", asks, allocator);
            
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            
            res.set_content(buffer.GetString(), "application/json");
            res.set_header("Cache-Control", "no-cache");
        } else {
            res.status = 404;
            res.set_content("{\"error\":\"Symbol not found\"}", "application/json");
        }
    }
};
```

### Rate Limiting
```cpp
class RateLimiter {
private:
    struct ClientRateInfo {
        std::atomic<uint64_t> request_count{0};
        std::atomic<uint64_t> window_start{0};
    };
    
    std::unordered_map<std::string, ClientRateInfo> client_rates;
    const uint64_t max_requests_per_window = 100;
    const uint64_t window_size_ms = 1000;
    
public:
    bool allow_request(const std::string& client_ip) {
        auto& rate_info = client_rates[client_ip];
        uint64_t now = get_timestamp_ms();
        
        // Reset window if expired
        uint64_t window_start = rate_info.window_start.load();
        if (now - window_start > window_size_ms) {
            rate_info.window_start.store(now);
            rate_info.request_count.store(1);
            return true;
        }
        
        // Check rate limit
        uint64_t count = rate_info.request_count.fetch_add(1);
        return count < max_requests_per_window;
    }
};
```

## Performance Optimizations

### Zero-Copy Broadcasting
```cpp
class ZeroCopyBroadcaster {
private:
    // Pre-serialized message cache
    struct CachedMessage {
        std::string binary_data;
        std::string json_data;
        uint64_t timestamp;
    };
    
    LRUCache<std::string, CachedMessage> message_cache{1000};
    
public:
    void broadcast_to_clients(const OrderBookUpdate& update,
                              const std::vector<ClientSession*>& clients) {
        // Check cache
        auto cache_key = generate_cache_key(update);
        auto cached = message_cache.get(cache_key);
        
        if (!cached || cached->timestamp < update.timestamp) {
            // Serialize once
            CachedMessage msg;
            msg.binary_data = serialize_binary(update);
            msg.json_data = serialize_json(update);
            msg.timestamp = update.timestamp;
            
            message_cache.put(cache_key, msg);
            cached = &msg;
        }
        
        // Send to all clients (zero-copy)
        for (auto* client : clients) {
            if (client->use_binary) {
                send_binary(client, cached->binary_data);
            } else {
                send_text(client, cached->json_data);
            }
        }
    }
};
```

### Connection Pooling
```cpp
class ConnectionPooling {
private:
    struct PooledConnection {
        websocketpp::connection_ptr connection;
        std::chrono::steady_clock::time_point last_used;
        bool in_use = false;
    };
    
    std::vector<PooledConnection> connection_pool;
    std::mutex pool_mutex;
    
public:
    websocketpp::connection_ptr get_connection() {
        std::lock_guard<std::mutex> lock(pool_mutex);
        
        for (auto& conn : connection_pool) {
            if (!conn.in_use && conn.connection->get_state() == websocketpp::session::state::open) {
                conn.in_use = true;
                conn.last_used = std::chrono::steady_clock::now();
                return conn.connection;
            }
        }
        
        // Create new connection if pool exhausted
        return create_new_connection();
    }
    
    void return_connection(websocketpp::connection_ptr conn) {
        std::lock_guard<std::mutex> lock(pool_mutex);
        
        auto it = std::find_if(connection_pool.begin(), connection_pool.end(),
            [&conn](const PooledConnection& pc) {
                return pc.connection == conn;
            });
        
        if (it != connection_pool.end()) {
            it->in_use = false;
        }
    }
};
```

## Security

### Authentication
```cpp
class AuthenticationManager {
private:
    std::unordered_map<std::string, std::string> api_keys;
    
public:
    bool authenticate_websocket(const std::string& token) {
        // Validate JWT token
        auto decoded = jwt::decode(token);
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{"secret"})
            .with_issuer("market-data-server");
        
        try {
            verifier.verify(decoded);
            return true;
        } catch (const std::exception& e) {
            return false;
        }
    }
    
    bool authenticate_api(const std::string& api_key) {
        return api_keys.find(api_key) != api_keys.end();
    }
};
```

## Monitoring and Metrics

### Prometheus Metrics Export
```cpp
class PrometheusExporter {
private:
    prometheus::Registry registry;
    
    prometheus::Counter& ws_connections_total;
    prometheus::Gauge& ws_connections_active;
    prometheus::Histogram& message_broadcast_latency;
    prometheus::Counter& api_requests_total;
    
public:
    std::string export_metrics() {
        std::ostringstream stream;
        prometheus::TextSerializer serializer;
        serializer.Serialize(stream, registry.Collect());
        return stream.str();
    }
};
```
