# Exchange Connectors Implementation Specification

## Overview
Implement high-performance WebSocket and REST API connectors for Binance, Coinbase, and Kraken exchanges with automatic reconnection, rate limiting, and message normalization.

## Base Connector Interface

```cpp
class IExchangeConnector {
public:
    virtual ~IExchangeConnector() = default;
    
    // Lifecycle management
    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    
    // Subscription management
    virtual void subscribe_order_book(const std::string& symbol, int depth = 20) = 0;
    virtual void subscribe_trades(const std::string& symbol) = 0;
    virtual void unsubscribe(const std::string& symbol) = 0;
    
    // Message handling
    virtual void set_message_handler(MessageHandler handler) = 0;
    virtual void set_error_handler(ErrorHandler handler) = 0;
    
    // Statistics
    virtual ConnectorStats get_stats() const = 0;
};
```

## Binance Connector

### WebSocket Endpoints
```
wss://stream.binance.com:9443/ws/<symbol>@depth@100ms  // Order book updates
wss://stream.binance.com:9443/ws/<symbol>@trade         // Trade stream
wss://stream.binance.com:9443/stream?streams=           // Combined streams
```

### Message Format
```cpp
struct BinanceDepthUpdate {
    char event_type[10];        // "depthUpdate"
    uint64_t event_time;         // Event time
    char symbol[20];             // Symbol
    uint64_t first_update_id;    // First update ID in event
    uint64_t final_update_id;    // Final update ID in event
    
    struct PriceLevel {
        char price[32];          // Price as string
        char quantity[32];       // Quantity as string
    };
    
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};
```

### Implementation Details
```cpp
class BinanceConnector : public IExchangeConnector {
private:
    // WebSocket client with SSL support
    websocketpp::client<websocketpp::config::asio_tls_client> ws_client;
    websocketpp::connection_hdl connection;
    
    // Message parsing
    simdjson::ondemand::parser json_parser;
    
    // Rate limiting
    TokenBucket rate_limiter{1200, 60}; // 1200 requests per minute
    
    // Connection management
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> last_ping{0};
    std::thread heartbeat_thread;
    
    // Subscription tracking
    std::unordered_set<std::string> subscribed_symbols;
    std::mutex subscription_mutex;
    
    // Reconnection logic
    ExponentialBackoff backoff{100ms, 30s};
    
public:
    void connect() override {
        ws_client.set_tls_init_handler([](connection_hdl) {
            auto ctx = websocketpp::lib::make_shared<ssl::context>(ssl::context::tlsv12);
            ctx->set_options(ssl::context::default_workarounds |
                           ssl::context::no_sslv2 |
                           ssl::context::no_sslv3 |
                           ssl::context::single_dh_use);
            return ctx;
        });
        
        ws_client.set_message_handler(
            [this](connection_hdl, message_ptr msg) {
                process_message(msg->get_payload());
            }
        );
        
        ws_client.set_close_handler(
            [this](connection_hdl) {
                handle_disconnect();
            }
        );
        
        // Connect with retry logic
        while (!connected) {
            try {
                auto con = ws_client.get_connection("wss://stream.binance.com:9443/stream");
                connection = con->get_handle();
                ws_client.connect(con);
                ws_client.run();
                connected = true;
            } catch (const std::exception& e) {
                auto delay = backoff.next_delay();
                std::this_thread::sleep_for(delay);
            }
        }
        
        start_heartbeat();
    }
    
    void process_message(const std::string& raw_message) {
        // Parse with simdjson (zero-copy where possible)
        simdjson::padded_string padded(raw_message);
        simdjson::ondemand::document doc = json_parser.iterate(padded);
        
        std::string_view event_type = doc["e"];
        
        if (event_type == "depthUpdate") {
            process_depth_update(doc);
        } else if (event_type == "trade") {
            process_trade(doc);
        }
    }
    
    void process_depth_update(simdjson::ondemand::document& doc) {
        NormalizedOrderBookUpdate update;
        update.exchange = Exchange::BINANCE;
        update.symbol = doc["s"];
        update.timestamp = doc["E"].get_uint64() * 1000000; // Convert to nanoseconds
        update.update_id = doc["u"];
        
        // Process bids (use SIMD-optimized parsing)
        auto bids = doc["b"].get_array();
        for (auto bid : bids) {
            auto bid_array = bid.get_array();
            auto it = bid_array.begin();
            
            double price = parse_decimal_fast((*it).get_string());
            ++it;
            double quantity = parse_decimal_fast((*it).get_string());
            
            update.bids.emplace_back(price, quantity);
        }
        
        // Process asks similarly
        auto asks = doc["a"].get_array();
        for (auto ask : asks) {
            auto ask_array = ask.get_array();
            auto it = ask_array.begin();
            
            double price = parse_decimal_fast((*it).get_string());
            ++it;
            double quantity = parse_decimal_fast((*it).get_string());
            
            update.asks.emplace_back(price, quantity);
        }
        
        // Send to handler
        if (message_handler) {
            message_handler(std::move(update));
        }
    }
};
```

## Coinbase Connector

### WebSocket Endpoints
```
wss://ws-feed.exchange.coinbase.com     // Public feed
wss://ws-feed-public.sandbox.exchange.coinbase.com  // Sandbox
```

### Message Format
```cpp
struct CoinbaseL2Update {
    char type[20];           // "l2update"
    char product_id[20];     // "BTC-USD"
    uint64_t time;           // RFC3339 timestamp
    
    struct Change {
        char side[5];        // "buy" or "sell"
        char price[32];
        char size[32];
    };
    
    std::vector<Change> changes;
};
```

### Implementation
```cpp
class CoinbaseConnector : public IExchangeConnector {
private:
    // Coinbase uses different message format
    void subscribe_order_book(const std::string& symbol, int depth) override {
        nlohmann::json subscribe_msg = {
            {"type", "subscribe"},
            {"product_ids", {symbol}},
            {"channels", {
                {
                    {"name", "level2"},
                    {"product_ids", {symbol}}
                }
            }}
        };
        
        ws_client.send(connection, subscribe_msg.dump(), 
                      websocketpp::frame::opcode::text);
    }
    
    void process_l2_update(simdjson::ondemand::document& doc) {
        NormalizedOrderBookUpdate update;
        update.exchange = Exchange::COINBASE;
        update.symbol = doc["product_id"];
        
        // Parse ISO 8601 timestamp
        std::string time_str = doc["time"];
        update.timestamp = parse_iso8601_to_nanos(time_str);
        
        auto changes = doc["changes"].get_array();
        for (auto change : changes) {
            auto change_array = change.get_array();
            auto it = change_array.begin();
            
            std::string_view side = (*it).get_string();
            ++it;
            double price = parse_decimal_fast((*it).get_string());
            ++it;
            double size = parse_decimal_fast((*it).get_string());
            
            if (side == "buy") {
                update.bids.emplace_back(price, size);
            } else {
                update.asks.emplace_back(price, size);
            }
        }
        
        if (message_handler) {
            message_handler(std::move(update));
        }
    }
};
```

## Kraken Connector

### WebSocket Endpoints
```
wss://ws.kraken.com                    // Public
wss://ws-auth.kraken.com               // Authenticated
```

### Implementation
```cpp
class KrakenConnector : public IExchangeConnector {
private:
    void subscribe_order_book(const std::string& symbol, int depth) override {
        nlohmann::json subscribe_msg = {
            {"event", "subscribe"},
            {"pair", {symbol}},
            {"subscription", {
                {"name", "book"},
                {"depth", depth}
            }}
        };
        
        ws_client.send(connection, subscribe_msg.dump(),
                      websocketpp::frame::opcode::text);
    }
    
    void process_book_update(simdjson::ondemand::document& doc) {
        // Kraken sends arrays directly
        auto data = doc.get_array();
        auto it = data.begin();
        
        ++it; // Skip channel ID
        
        NormalizedOrderBookUpdate update;
        update.exchange = Exchange::KRAKEN;
        
        // Parse bid/ask updates
        auto book_data = (*it).get_object();
        
        // Process bids
        if (book_data.find("bs") != book_data.end()) {
            auto bids = book_data["bs"].get_array();
            for (auto bid : bids) {
                auto bid_array = bid.get_array();
                auto bid_it = bid_array.begin();
                
                double price = parse_decimal_fast((*bid_it).get_string());
                ++bid_it;
                double volume = parse_decimal_fast((*bid_it).get_string());
                
                update.bids.emplace_back(price, volume);
            }
        }
        
        // Process asks similarly
        if (book_data.find("as") != book_data.end()) {
            auto asks = book_data["as"].get_array();
            for (auto ask : asks) {
                auto ask_array = ask.get_array();
                auto ask_it = ask_array.begin();
                
                double price = parse_decimal_fast((*ask_it).get_string());
                ++ask_it;
                double volume = parse_decimal_fast((*ask_it).get_string());
                
                update.asks.emplace_back(price, volume);
            }
        }
        
        if (message_handler) {
            message_handler(std::move(update));
        }
    }
};
```

## Common Components

### Fast Decimal Parser
```cpp
// Optimized decimal string to double conversion
inline double parse_decimal_fast(std::string_view str) {
    double result = 0.0;
    double decimal = 0.0;
    double divisor = 10.0;
    bool is_decimal = false;
    
    for (char c : str) {
        if (c == '.') {
            is_decimal = true;
        } else if (c >= '0' && c <= '9') {
            if (!is_decimal) {
                result = result * 10 + (c - '0');
            } else {
                decimal += (c - '0') / divisor;
                divisor *= 10;
            }
        }
    }
    
    return result + decimal;
}
```

### Rate Limiter
```cpp
class TokenBucket {
private:
    std::atomic<uint64_t> tokens;
    std::atomic<uint64_t> last_refill;
    const uint64_t capacity;
    const uint64_t refill_rate_per_second;
    
public:
    TokenBucket(uint64_t capacity, uint64_t refill_rate)
        : tokens(capacity)
        , last_refill(get_timestamp_ns())
        , capacity(capacity)
        , refill_rate_per_second(refill_rate) {}
    
    bool try_consume(uint64_t count = 1) {
        refill();
        
        uint64_t current = tokens.load();
        while (current >= count) {
            if (tokens.compare_exchange_weak(current, current - count)) {
                return true;
            }
        }
        return false;
    }
    
    void refill() {
        uint64_t now = get_timestamp_ns();
        uint64_t last = last_refill.load();
        uint64_t elapsed_ns = now - last;
        
        if (elapsed_ns > 1000000000) { // More than 1 second
            uint64_t new_tokens = (elapsed_ns * refill_rate_per_second) / 1000000000;
            uint64_t current = tokens.load();
            uint64_t new_total = std::min(current + new_tokens, capacity);
            
            tokens.store(new_total);
            last_refill.store(now);
        }
    }
};
```

### Reconnection Strategy
```cpp
class ExponentialBackoff {
private:
    std::chrono::milliseconds current_delay;
    const std::chrono::milliseconds min_delay;
    const std::chrono::milliseconds max_delay;
    std::mt19937 rng{std::random_device{}()};
    
public:
    ExponentialBackoff(std::chrono::milliseconds min_d,
                      std::chrono::milliseconds max_d)
        : current_delay(min_d), min_delay(min_d), max_delay(max_d) {}
    
    std::chrono::milliseconds next_delay() {
        auto delay = current_delay;
        
        // Add jitter (±25%)
        std::uniform_int_distribution<> dist(-25, 25);
        auto jitter = delay * dist(rng) / 100;
        delay += jitter;
        
        // Exponential increase
        current_delay = std::min(current_delay * 2, max_delay);
        
        return delay;
    }
    
    void reset() {
        current_delay = min_delay;
    }
};
```

## Message Normalization

### Unified Message Format
```cpp
struct NormalizedOrderBookUpdate {
    Exchange exchange;
    std::string symbol;
    uint64_t timestamp;     // Nanoseconds since epoch
    uint64_t update_id;     // Exchange-specific update ID
    
    struct PriceLevel {
        double price;
        double quantity;
        
        PriceLevel(double p, double q) : price(p), quantity(q) {}
    };
    
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};

struct NormalizedTrade {
    Exchange exchange;
    std::string symbol;
    uint64_t timestamp;     // Nanoseconds since epoch
    std::string trade_id;
    double price;
    double quantity;
    bool is_buyer_maker;    // true if buyer was maker
};
```

## Error Handling

### Connection Errors
```cpp
enum class ConnectionError {
    NETWORK_ERROR,
    AUTHENTICATION_FAILED,
    RATE_LIMIT_EXCEEDED,
    INVALID_MESSAGE,
    EXCHANGE_ERROR
};

class ConnectionException : public std::exception {
private:
    ConnectionError error_type;
    std::string message;
    
public:
    ConnectionException(ConnectionError type, const std::string& msg)
        : error_type(type), message(msg) {}
    
    const char* what() const noexcept override {
        return message.c_str();
    }
    
    ConnectionError type() const { return error_type; }
};
```

### Error Recovery
```cpp
void handle_error(const ConnectionException& e) {
    switch (e.type()) {
        case ConnectionError::NETWORK_ERROR:
            // Attempt reconnection with backoff
            schedule_reconnect();
            break;
            
        case ConnectionError::RATE_LIMIT_EXCEEDED:
            // Wait and retry with reduced rate
            std::this_thread::sleep_for(60s);
            reduce_request_rate();
            break;
            
        case ConnectionError::INVALID_MESSAGE:
            // Log and continue
            logger->error("Invalid message: {}", e.what());
            break;
            
        default:
            // Log and alert
            logger->critical("Critical error: {}", e.what());
            alert_handler(e);
    }
}
```

## Performance Monitoring

### Metrics Collection
```cpp
struct ConnectorStats {
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> messages_processed{0};
    std::atomic<uint64_t> parse_errors{0};
    std::atomic<uint64_t> reconnections{0};
    
    // Latency histogram
    LockFreeHistogram<1000> message_latency_us;
    
    // Throughput
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> bytes_sent{0};
    
    // Connection health
    std::atomic<uint64_t> last_message_time{0};
    std::atomic<bool> is_healthy{true};
};
```

## Testing

### Mock Exchange Server
```cpp
class MockExchangeServer {
private:
    websocketpp::server<websocketpp::config::asio> server;
    std::vector<std::string> playback_messages;
    
public:
    void start_playback(const std::string& recording_file) {
        // Load recorded messages
        load_messages(recording_file);
        
        // Set up server handlers
        server.set_message_handler(
            [this](connection_hdl hdl, server::message_ptr msg) {
                handle_client_message(hdl, msg);
            }
        );
        
        // Start server
        server.listen(9002);
        server.start_accept();
        server.run();
    }
    
    void send_burst(int messages_per_second) {
        // Send messages at specified rate for testing
    }
};
```
