# Implementation Details and Testing Specification

## CMake Build Configuration

### Main CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.20)
project(MarketDataHandler VERSION 1.0.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -mtune=native -flto")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Link-time optimization
include(CheckIPOSupported)
check_ipo_supported(RESULT supported OUTPUT error)
if(supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Find packages
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost 1.75 REQUIRED COMPONENTS system thread)
find_package(Protobuf REQUIRED)
find_package(GTest REQUIRED)
find_package(benchmark REQUIRED)

# Include FetchContent for dependencies
include(FetchContent)

# Fetch simdjson
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.0.0
)
FetchContent_MakeAvailable(simdjson)

# Fetch websocketpp
FetchContent_Declare(
    websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG 0.8.2
)
FetchContent_MakeAvailable(websocketpp)

# Fetch spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.11.0
)
FetchContent_MakeAvailable(spdlog)

# Generate protobuf files
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
    proto/market_data.proto
)

# Main library
add_library(market_data_core
    src/core/order_book.cpp
    src/core/memory_pool.cpp
    src/core/lock_free_queue.cpp
    src/exchange/binance_connector.cpp
    src/exchange/coinbase_connector.cpp
    src/exchange/kraken_connector.cpp
    src/network/websocket_server.cpp
    src/network/rest_api.cpp
    src/metrics/metrics_collector.cpp
    ${PROTO_SRCS}
)

target_include_directories(market_data_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}  # For protobuf headers
)

target_link_libraries(market_data_core PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::system
    Boost::thread
    ${Protobuf_LIBRARIES}
    simdjson::simdjson
    spdlog::spdlog
)

# Main executable
add_executable(market_data_handler
    src/main.cpp
)

target_link_libraries(market_data_handler PRIVATE
    market_data_core
)

# Tests
enable_testing()

add_executable(unit_tests
    tests/unit/test_order_book.cpp
    tests/unit/test_lock_free_queue.cpp
    tests/unit/test_memory_pool.cpp
    tests/unit/test_connectors.cpp
)

target_link_libraries(unit_tests PRIVATE
    market_data_core
    GTest::GTest
    GTest::Main
)

add_test(NAME unit_tests COMMAND unit_tests)

# Benchmarks
add_executable(benchmarks
    tests/benchmarks/bench_order_book.cpp
    tests/benchmarks/bench_serialization.cpp
    tests/benchmarks/bench_broadcasting.cpp
)

target_link_libraries(benchmarks PRIVATE
    market_data_core
    benchmark::benchmark
)

# Installation
install(TARGETS market_data_handler
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)

# CPack
set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
```

## Main Application Entry Point

### src/main.cpp
```cpp
#include <iostream>
#include <signal.h>
#include <thread>
#include <atomic>
#include "core/order_book_manager.hpp"
#include "exchange/exchange_manager.hpp"
#include "network/websocket_server.hpp"
#include "network/rest_api.hpp"
#include "metrics/metrics_collector.hpp"
#include "utils/config_loader.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/async.h>

std::atomic<bool> running{true};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        spdlog::info("Received shutdown signal");
        running = false;
    }
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return 1;
    }
    
    // Initialize logging
    auto logger = spdlog::rotating_logger_mt<spdlog::async_factory>(
        "market_data",
        "logs/market_data.log",
        1024 * 1024 * 100,  // 100 MB
        5                    // 5 files
    );
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_every(std::chrono::seconds(1));
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        // Load configuration
        auto config = ConfigLoader::load(argv[1]);
        
        spdlog::info("Starting Market Data Handler v{}.{}.{}",
                     PROJECT_VERSION_MAJOR,
                     PROJECT_VERSION_MINOR,
                     PROJECT_VERSION_PATCH);
        
        // Initialize components
        OrderBookManager order_book_manager(config.order_book);
        MetricsCollector metrics_collector;
        
        // Initialize exchange connectors
        ExchangeManager exchange_manager(order_book_manager, metrics_collector);
        
        if (config.exchanges.binance_enabled) {
            exchange_manager.add_connector(
                std::make_unique<BinanceConnector>(config.binance)
            );
        }
        
        if (config.exchanges.coinbase_enabled) {
            exchange_manager.add_connector(
                std::make_unique<CoinbaseConnector>(config.coinbase)
            );
        }
        
        if (config.exchanges.kraken_enabled) {
            exchange_manager.add_connector(
                std::make_unique<KrakenConnector>(config.kraken)
            );
        }
        
        // Start exchange connections
        exchange_manager.connect_all();
        
        // Subscribe to symbols
        for (const auto& symbol : config.symbols) {
            exchange_manager.subscribe_all(symbol);
        }
        
        // Initialize WebSocket server
        MarketDataWebSocketServer ws_server(
            order_book_manager,
            metrics_collector,
            config.websocket_server
        );
        
        // Initialize REST API
        MarketDataRestAPI rest_api(
            order_book_manager,
            metrics_collector,
            config.rest_api
        );
        
        // Start servers in separate threads
        std::thread ws_thread([&ws_server]() {
            ws_server.start();
        });
        
        std::thread api_thread([&rest_api]() {
            rest_api.start();
        });
        
        // Set CPU affinity for main thread
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        
        // Main loop
        while (running) {
            // Log metrics every second
            auto stats = metrics_collector.get_summary();
            spdlog::info("Stats - Messages/sec: {}, Active connections: {}, "
                        "Latency p99: {} us",
                        stats.messages_per_second,
                        stats.active_connections,
                        stats.latency_p99_us);
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // Graceful shutdown
        spdlog::info("Shutting down...");
        
        exchange_manager.disconnect_all();
        ws_server.stop();
        rest_api.stop();
        
        ws_thread.join();
        api_thread.join();
        
        spdlog::info("Shutdown complete");
        
    } catch (const std::exception& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    }
    
    return 0;
}
```

## Configuration File Format

### config/config.yaml
```yaml
# Market Data Handler Configuration

# Logging configuration
logging:
  level: info  # debug, info, warning, error, critical
  file: logs/market_data.log
  max_size_mb: 100
  max_files: 5
  async: true

# Order book configuration
order_book:
  max_levels: 1000
  snapshot_interval_ms: 100
  enable_statistics: true

# Exchange configurations
exchanges:
  binance_enabled: true
  coinbase_enabled: true
  kraken_enabled: true

binance:
  ws_endpoint: wss://stream.binance.com:9443/stream
  rest_endpoint: https://api.binance.com
  rate_limit: 1200  # requests per minute
  reconnect_delay_ms: 1000
  max_reconnect_delay_ms: 30000

coinbase:
  ws_endpoint: wss://ws-feed.exchange.coinbase.com
  rest_endpoint: https://api.exchange.coinbase.com
  rate_limit: 100
  reconnect_delay_ms: 1000
  max_reconnect_delay_ms: 30000

kraken:
  ws_endpoint: wss://ws.kraken.com
  rest_endpoint: https://api.kraken.com
  rate_limit: 60
  reconnect_delay_ms: 1000
  max_reconnect_delay_ms: 30000

# Symbols to subscribe to
symbols:
  - BTC-USD
  - ETH-USD
  - BTC-USDT
  - ETH-USDT

# WebSocket server configuration
websocket_server:
  port: 8080
  max_connections: 10000
  ping_interval_ms: 30000
  connection_timeout_ms: 60000
  use_compression: true
  use_binary_protocol: true
  tls_enabled: true
  cert_file: certs/server.crt
  key_file: certs/server.key

# REST API configuration
rest_api:
  port: 8081
  max_requests_per_second: 1000
  enable_cors: true
  cors_origins:
    - http://localhost:3000
    - https://yourdomain.com
  rate_limit_per_ip: 100
  cache_ttl_ms: 100

# Metrics configuration
metrics:
  enable_prometheus: true
  prometheus_port: 9090
  export_interval_ms: 1000

# Performance tuning
performance:
  thread_pool_size: 8
  use_huge_pages: true
  numa_nodes: 1
  cpu_affinity:
    main_thread: 0
    exchange_threads: [1, 2, 3]
    broadcast_thread: 4
    api_threads: [5, 6]
```

## Unit Tests

### tests/unit/test_order_book.cpp
```cpp
#include <gtest/gtest.h>
#include "core/order_book.hpp"
#include <thread>
#include <random>

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook<100> book;
    
    void SetUp() override {
        // Initialize with some data
        book.update_bid(100.0, 10.0);
        book.update_bid(99.5, 20.0);
        book.update_ask(100.5, 15.0);
        book.update_ask(101.0, 25.0);
    }
};

TEST_F(OrderBookTest, UpdateBid) {
    book.update_bid(99.0, 30.0);
    
    auto snapshot = book.get_snapshot(5);
    ASSERT_EQ(snapshot.bids.size(), 3);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 100.0);
    EXPECT_DOUBLE_EQ(snapshot.bids[1].price, 99.5);
    EXPECT_DOUBLE_EQ(snapshot.bids[2].price, 99.0);
}

TEST_F(OrderBookTest, UpdateAsk) {
    book.update_ask(102.0, 35.0);
    
    auto snapshot = book.get_snapshot(5);
    ASSERT_EQ(snapshot.asks.size(), 3);
    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 100.5);
    EXPECT_DOUBLE_EQ(snapshot.asks[1].price, 101.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[2].price, 102.0);
}

TEST_F(OrderBookTest, RemoveLevel) {
    book.update_bid(100.0, 0.0);  // Remove level
    
    auto snapshot = book.get_snapshot(5);
    ASSERT_EQ(snapshot.bids.size(), 1);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 99.5);
}

TEST_F(OrderBookTest, ConcurrentUpdates) {
    const int num_threads = 10;
    const int updates_per_thread = 10000;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([this, i, updates_per_thread]() {
            std::mt19937 gen(i);
            std::uniform_real_distribution<> price_dist(90.0, 110.0);
            std::uniform_real_distribution<> qty_dist(0.0, 100.0);
            
            for (int j = 0; j < updates_per_thread; ++j) {
                double price = price_dist(gen);
                double qty = qty_dist(gen);
                
                if (price < 100.0) {
                    book.update_bid(price, qty);
                } else {
                    book.update_ask(price, qty);
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Verify consistency
    auto snapshot = book.get_snapshot(100);
    
    // Bids should be sorted descending
    for (size_t i = 1; i < snapshot.bids.size(); ++i) {
        EXPECT_GE(snapshot.bids[i-1].price, snapshot.bids[i].price);
    }
    
    // Asks should be sorted ascending
    for (size_t i = 1; i < snapshot.asks.size(); ++i) {
        EXPECT_LE(snapshot.asks[i-1].price, snapshot.asks[i].price);
    }
    
    // Best bid should be less than best ask
    if (!snapshot.bids.empty() && !snapshot.asks.empty()) {
        EXPECT_LT(snapshot.bids[0].price, snapshot.asks[0].price);
    }
}

TEST_F(OrderBookTest, GetSpread) {
    double spread = book.get_spread();
    EXPECT_DOUBLE_EQ(spread, 0.5);  // 100.5 - 100.0
}

TEST_F(OrderBookTest, GetMidPrice) {
    double mid = book.get_mid_price();
    EXPECT_DOUBLE_EQ(mid, 100.25);  // (100.0 + 100.5) / 2
}

TEST_F(OrderBookTest, GetBookImbalance) {
    double imbalance = book.get_book_imbalance();
    // (10 + 20) - (15 + 25) = -10
    // -10 / 70 = -0.142857...
    EXPECT_NEAR(imbalance, -0.142857, 0.000001);
}
```

## Benchmark Tests

### tests/benchmarks/bench_order_book.cpp
```cpp
#include <benchmark/benchmark.h>
#include "core/order_book.hpp"
#include <random>

static void BM_OrderBookUpdate(benchmark::State& state) {
    OrderBook<1000> book;
    std::mt19937 gen(42);
    std::uniform_real_distribution<> price_dist(90.0, 110.0);
    std::uniform_real_distribution<> qty_dist(0.0, 100.0);
    
    for (auto _ : state) {
        double price = price_dist(gen);
        double qty = qty_dist(gen);
        
        if (price < 100.0) {
            book.update_bid(price, qty);
        } else {
            book.update_ask(price, qty);
        }
    }
    
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookUpdate);

static void BM_OrderBookSnapshot(benchmark::State& state) {
    OrderBook<1000> book;
    
    // Pre-populate order book
    for (double price = 90.0; price < 100.0; price += 0.1) {
        book.update_bid(price, price * 10);
    }
    for (double price = 100.0; price < 110.0; price += 0.1) {
        book.update_ask(price, price * 10);
    }
    
    for (auto _ : state) {
        auto snapshot = book.get_snapshot(20);
        benchmark::DoNotOptimize(snapshot);
    }
    
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookSnapshot);

static void BM_ConcurrentOrderBookUpdates(benchmark::State& state) {
    OrderBook<1000> book;
    
    for (auto _ : state) {
        state.PauseTiming();
        std::vector<std::thread> threads;
        std::atomic<bool> start{false};
        
        for (int i = 0; i < state.range(0); ++i) {
            threads.emplace_back([&book, &start, i]() {
                while (!start.load()) {
                    std::this_thread::yield();
                }
                
                std::mt19937 gen(i);
                std::uniform_real_distribution<> price_dist(90.0, 110.0);
                std::uniform_real_distribution<> qty_dist(0.0, 100.0);
                
                for (int j = 0; j < 1000; ++j) {
                    double price = price_dist(gen);
                    double qty = qty_dist(gen);
                    
                    if (price < 100.0) {
                        book.update_bid(price, qty);
                    } else {
                        book.update_ask(price, qty);
                    }
                }
            });
        }
        
        state.ResumeTiming();
        start.store(true);
        
        for (auto& t : threads) {
            t.join();
        }
    }
    
    state.SetItemsProcessed(state.iterations() * state.range(0) * 1000);
}
BENCHMARK(BM_ConcurrentOrderBookUpdates)->Range(1, 16);

BENCHMARK_MAIN();
```

## Deployment Scripts

### scripts/build.sh
```bash
#!/bin/bash

# Build script for Market Data Handler

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

echo -e "${GREEN}Building Market Data Handler...${NC}"

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo -e "${GREEN}Configuring...${NC}"
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_INSTALL_PREFIX=/opt/market-data-handler

# Build
echo -e "${GREEN}Compiling...${NC}"
make -j$(nproc)

# Run tests
echo -e "${GREEN}Running tests...${NC}"
ctest --output-on-failure

# Run benchmarks
echo -e "${GREEN}Running benchmarks...${NC}"
./benchmarks --benchmark_min_time=1

echo -e "${GREEN}Build complete!${NC}"
```

### scripts/deploy.sh
```bash
#!/bin/bash

# Deployment script

set -e

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root"
    exit 1
fi

# Install
cd build
make install

# Create systemd service
cat > /etc/systemd/system/market-data-handler.service <<EOF
[Unit]
Description=Market Data Handler
After=network.target

[Service]
Type=simple
User=market-data
Group=market-data
ExecStart=/opt/market-data-handler/bin/market_data_handler /opt/market-data-handler/config/config.yaml
Restart=always
RestartSec=10
LimitNOFILE=65535
LimitNPROC=4096

# Performance settings
CPUAffinity=0-7
Nice=-10

[Install]
WantedBy=multi-user.target
EOF

# Create user
useradd -r -s /bin/false market-data || true

# Create directories
mkdir -p /opt/market-data-handler/{logs,config}
chown -R market-data:market-data /opt/market-data-handler

# Copy configuration
cp ../config/config.yaml /opt/market-data-handler/config/

# Reload systemd
systemctl daemon-reload
systemctl enable market-data-handler
systemctl start market-data-handler

echo "Deployment complete!"
```

## Performance Tuning Guide

### Linux Kernel Parameters
```bash
# /etc/sysctl.conf

# Network tuning
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.core.netdev_max_backlog = 5000
net.ipv4.tcp_rmem = 4096 87380 134217728
net.ipv4.tcp_wmem = 4096 65536 134217728
net.ipv4.tcp_no_metrics_save = 1
net.ipv4.tcp_congestion_control = bbr

# Memory tuning
vm.swappiness = 10
vm.dirty_ratio = 15
vm.dirty_background_ratio = 5

# File system
fs.file-max = 2097152
fs.nr_open = 2097152
```

### CPU Isolation
```bash
# /etc/default/grub
GRUB_CMDLINE_LINUX="isolcpus=2,3,4,5 nohz_full=2,3,4,5 rcu_nocbs=2,3,4,5"
```

### Huge Pages
```bash
# Enable transparent huge pages
echo always > /sys/kernel/mm/transparent_hugepage/enabled
echo always > /sys/kernel/mm/transparent_hugepage/defrag

# Or use static huge pages
echo 1024 > /proc/sys/vm/nr_hugepages
```
