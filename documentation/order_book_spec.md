# Order Book Implementation Specification

## Overview
Implement a high-performance, lock-free order book that maintains bid and ask price levels with microsecond-level update latency.

## Data Structure Requirements

### Price Level Structure
```cpp
struct PriceLevel {
    std::atomic<uint64_t> price;      // Price in fixed-point (8 decimals)
    std::atomic<uint64_t> quantity;   // Total quantity at this level
    std::atomic<uint32_t> order_count; // Number of orders at this level
    std::atomic<uint64_t> timestamp;   // Last update timestamp (nanoseconds)
};
```

### Order Book Core Structure
```cpp
template<size_t MAX_LEVELS = 1000>
class OrderBook {
    // Separate arrays for bids and asks (cache-line aligned)
    alignas(64) std::array<PriceLevel, MAX_LEVELS> bids;
    alignas(64) std::array<PriceLevel, MAX_LEVELS> asks;
    
    // Atomic pointers to best bid/ask
    std::atomic<size_t> best_bid_idx;
    std::atomic<size_t> best_ask_idx;
    
    // Memory pool for updates
    LockFreePool<OrderUpdate> update_pool;
    
    // Ring buffer for recent trades
    RingBuffer<Trade, 10000> recent_trades;
    
    // Statistics
    alignas(64) Statistics stats;
};
```

## Lock-Free Algorithm Requirements

### Update Operations
1. **Add Order**: Insert new price level or update existing
2. **Remove Order**: Decrease quantity or remove level
3. **Modify Order**: Atomic update of quantity
4. **Clear Book**: Reset all levels atomically

### Concurrency Strategy
```cpp
// Use compare-and-swap (CAS) for updates
bool update_level(size_t idx, uint64_t new_quantity) {
    uint64_t expected = levels[idx].quantity.load();
    return levels[idx].quantity.compare_exchange_strong(
        expected, new_quantity, 
        std::memory_order_release,
        std::memory_order_acquire
    );
}

// Use sequence lock for consistent reads
class SeqLock {
    std::atomic<uint64_t> sequence{0};
    
    void write_lock() {
        sequence.fetch_add(1, std::memory_order_acquire);
    }
    
    void write_unlock() {
        sequence.fetch_add(1, std::memory_order_release);
    }
    
    uint64_t read_begin() {
        return sequence.load(std::memory_order_acquire);
    }
    
    bool read_retry(uint64_t seq) {
        return (seq & 1) || (sequence.load(std::memory_order_acquire) != seq);
    }
};
```

## Memory Layout Optimization

### Cache Line Alignment
- Each price level should fit in 1-2 cache lines
- Separate hot (frequently accessed) and cold data
- Use prefetching for sequential access patterns

### False Sharing Prevention
```cpp
struct alignas(64) PaddedAtomic {
    std::atomic<uint64_t> value;
    char padding[64 - sizeof(std::atomic<uint64_t>)];
};
```

## Fast Lookup Implementation

### Price to Index Mapping
```cpp
class PriceIndexMap {
    // Use open addressing hash table with linear probing
    struct Entry {
        std::atomic<uint64_t> price{0};
        std::atomic<size_t> index{SIZE_MAX};
    };
    
    static constexpr size_t TABLE_SIZE = 4096; // Power of 2
    alignas(64) std::array<Entry, TABLE_SIZE> table;
    
    size_t hash(uint64_t price) const {
        // Fast hash using multiplication
        return (price * 0x9E3779B97F4A7C15ULL) & (TABLE_SIZE - 1);
    }
    
    std::optional<size_t> find(uint64_t price) const;
    bool insert(uint64_t price, size_t index);
    bool remove(uint64_t price);
};
```

## Snapshot Generation

### Consistent Snapshot Algorithm
```cpp
struct OrderBookSnapshot {
    uint64_t timestamp;
    std::vector<PriceLevel> bid_levels;
    std::vector<PriceLevel> ask_levels;
    uint64_t sequence_number;
};

OrderBookSnapshot create_snapshot() {
    SeqLock::ReadGuard guard(seq_lock);
    OrderBookSnapshot snapshot;
    
    do {
        auto seq = guard.begin();
        // Copy visible levels
        copy_visible_levels(snapshot);
        // Retry if data changed during read
    } while (guard.retry(seq));
    
    return snapshot;
}
```

## Market Data Aggregation

### Multi-Exchange Book
```cpp
class AggregatedOrderBook {
    struct ExchangeLevel {
        uint64_t price;
        uint64_t quantity;
        ExchangeId exchange;
        uint64_t timestamp;
    };
    
    // Maintain sorted vectors for efficient aggregation
    std::vector<ExchangeLevel> aggregated_bids;
    std::vector<ExchangeLevel> aggregated_asks;
    
    // Fast updates using binary search + insertion
    void update_level(ExchangeId exchange, Side side, 
                     uint64_t price, uint64_t quantity);
    
    // Get best bid/ask across all exchanges
    std::optional<ExchangeLevel> get_best_bid() const;
    std::optional<ExchangeLevel> get_best_ask() const;
    
    // Calculate weighted mid-price
    double get_weighted_mid_price() const;
};
```

## Performance Optimizations

### Memory Pool
```cpp
template<typename T, size_t POOL_SIZE = 100000>
class LockFreePool {
    alignas(64) std::array<T, POOL_SIZE> storage;
    std::atomic<size_t> head{0};
    std::array<std::atomic<bool>, POOL_SIZE> in_use;
    
    T* allocate() {
        size_t idx = head.fetch_add(1) % POOL_SIZE;
        while (in_use[idx].exchange(true)) {
            idx = (idx + 1) % POOL_SIZE;
        }
        return &storage[idx];
    }
    
    void deallocate(T* ptr) {
        size_t idx = ptr - storage.data();
        in_use[idx].store(false);
    }
};
```

### SIMD Operations
```cpp
// Use AVX2 for bulk operations
#include <immintrin.h>

void clear_levels_simd(PriceLevel* levels, size_t count) {
    const __m256i zero = _mm256_setzero_si256();
    for (size_t i = 0; i < count; i += 4) {
        _mm256_store_si256((__m256i*)&levels[i], zero);
    }
}

// Find minimum price using SIMD
uint64_t find_min_price_simd(const uint64_t* prices, size_t count) {
    __m256i min_vec = _mm256_set1_epi64x(UINT64_MAX);
    for (size_t i = 0; i < count; i += 4) {
        __m256i prices_vec = _mm256_load_si256((__m256i*)&prices[i]);
        min_vec = _mm256_min_epu64(min_vec, prices_vec);
    }
    // Horizontal minimum
    return horizontal_min(min_vec);
}
```

## Statistics and Metrics

### Real-time Metrics
```cpp
struct OrderBookMetrics {
    // Latency tracking (lock-free histogram)
    LockFreeHistogram<1000> update_latency_ns;
    
    // Throughput counters
    std::atomic<uint64_t> updates_per_second{0};
    std::atomic<uint64_t> snapshots_generated{0};
    
    // Book statistics
    std::atomic<uint64_t> total_bid_volume{0};
    std::atomic<uint64_t> total_ask_volume{0};
    std::atomic<double> spread{0.0};
    std::atomic<double> mid_price{0.0};
    
    // Market quality indicators
    std::atomic<double> book_imbalance{0.0};  // (bid_vol - ask_vol) / (bid_vol + ask_vol)
    std::atomic<uint32_t> bid_depth{0};        // Number of bid levels
    std::atomic<uint32_t> ask_depth{0};        // Number of ask levels
};
```

## Testing Requirements

### Unit Tests
```cpp
TEST(OrderBook, ConcurrentUpdates) {
    OrderBook book;
    std::vector<std::thread> threads;
    
    // Launch multiple threads updating different price levels
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&book, i]() {
            for (int j = 0; j < 100000; ++j) {
                book.update_bid(10000 + i, j % 1000);
            }
        });
    }
    
    // Verify consistency after all updates
    for (auto& t : threads) t.join();
    ASSERT_TRUE(book.verify_consistency());
}
```

### Benchmarks
```cpp
BENCHMARK(OrderBookUpdate) {
    OrderBook book;
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 1000000; ++i) {
        book.update_bid(10000 + (i % 100), i);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
        end - start).count() / 1000000;
    
    EXPECT_LT(latency, 100); // < 100ns per update
}
```

## Integration Points

### Exchange Message Processing
```cpp
void process_binance_message(const BinanceDepthUpdate& update) {
    for (const auto& bid : update.bids) {
        order_book.update_bid(bid.price, bid.quantity);
    }
    for (const auto& ask : update.asks) {
        order_book.update_ask(ask.price, ask.quantity);
    }
}
```

### WebSocket Broadcasting
```cpp
void broadcast_update(const OrderBookUpdate& update) {
    // Serialize to binary format
    auto buffer = serialize_protobuf(update);
    
    // Send to all connected clients
    for (auto& client : websocket_clients) {
        client.send_binary(buffer);
    }
}
```

## Error Handling

### Consistency Checks
- Verify best bid < best ask
- Check for negative quantities
- Validate price within reasonable bounds
- Monitor for stale data (timestamp checks)
- Detect and log anomalies

### Recovery Mechanisms
- Periodic full book refresh from exchange
- Automatic reconnection on disconnect
- State persistence to disk for crash recovery
- Checksum validation for data integrity
