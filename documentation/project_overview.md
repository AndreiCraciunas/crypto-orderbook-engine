# Low-Latency Market Data Feed Handler - Project Overview

## Executive Summary
Build a high-performance C++ market data feed handler that connects to multiple cryptocurrency exchanges, normalizes their data streams, maintains in-memory order books, and provides both a high-speed API and WebSocket interface for frontend visualization.

## Core Requirements

### Performance Targets
- **Latency**: < 10 microseconds from message receipt to order book update
- **Throughput**: Handle 1,000,000+ messages per second
- **Memory**: Maintain order books for 100+ symbols with minimal allocation
- **Concurrency**: Lock-free data structures for multi-threaded access
- **Availability**: 99.99% uptime with automatic reconnection

### Supported Exchanges
1. **Binance** (WebSocket + REST API)
2. **Coinbase** (WebSocket + REST API)
3. **Kraken** (WebSocket + REST API)

### Key Technologies
- **Language**: C++20 (with C++23 features where available)
- **Build System**: CMake 3.20+
- **WebSocket Library**: websocketpp or Beast (Boost.Beast)
- **HTTP Library**: libcurl or Beast
- **JSON Parsing**: simdjson (fastest JSON parser)
- **Serialization**: Protocol Buffers or MessagePack
- **Logging**: spdlog with async mode
- **Testing**: Google Test + Google Benchmark
- **Memory Pool**: Custom allocators with tcmalloc/jemalloc
- **Networking**: epoll/io_uring on Linux, IOCP on Windows
- **Time**: std::chrono::high_resolution_clock with RDTSC for ultra-low latency

## Architecture Overview

### Component Design
```
┌─────────────────────────────────────────────────────────────┐
│                     Main Application                         │
├─────────────────────────────────────────────────────────────┤
│                    Configuration Manager                     │
├─────────────────────────────────────────────────────────────┤
│                     Metrics Collector                        │
├───────────┬────────────┬────────────┬──────────────────────┤
│  Binance  │  Coinbase  │   Kraken   │   Exchange Manager   │
│  Connector│  Connector │  Connector │                      │
├───────────┴────────────┴────────────┴──────────────────────┤
│                    Message Normalizer                        │
├─────────────────────────────────────────────────────────────┤
│                 Lock-Free Order Book Manager                 │
├─────────────────────────────────────────────────────────────┤
│              Market Data Aggregation Engine                  │
├─────────────────────────────────────────────────────────────┤
│         WebSocket Server          │      REST API Server    │
├───────────────────────────────────┴──────────────────────────┤
│                     Memory Pool Manager                      │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow
1. Exchange connectors receive raw market data via WebSocket
2. Messages are parsed using zero-copy techniques
3. Normalized messages are placed in lock-free queues
4. Order book manager updates in-memory books atomically
5. Aggregation engine combines multi-exchange data
6. Updates are broadcast to connected clients
7. Metrics are collected without impacting hot path

## Memory Architecture

### Zero-Allocation Hot Path
- Pre-allocate all memory at startup
- Use ring buffers for message passing
- Object pools for order book entries
- Memory-mapped files for persistence
- Custom allocators aligned to cache lines (64 bytes)

### Cache Optimization
- Arrange data structures to minimize cache misses
- Use prefetching hints for predictable access patterns
- Separate hot and cold data
- NUMA-aware memory allocation

## Project Structure
```
market-data-handler/
├── CMakeLists.txt
├── cmake/
│   ├── Dependencies.cmake
│   └── CompilerOptions.cmake
├── include/
│   ├── core/
│   │   ├── order_book.hpp
│   │   ├── types.hpp
│   │   └── memory_pool.hpp
│   ├── exchange/
│   │   ├── connector_interface.hpp
│   │   ├── binance_connector.hpp
│   │   ├── coinbase_connector.hpp
│   │   └── kraken_connector.hpp
│   ├── network/
│   │   ├── websocket_client.hpp
│   │   ├── rest_client.hpp
│   │   └── websocket_server.hpp
│   ├── utils/
│   │   ├── lock_free_queue.hpp
│   │   ├── ring_buffer.hpp
│   │   └── time_utils.hpp
│   └── metrics/
│       └── metrics_collector.hpp
├── src/
│   ├── core/
│   ├── exchange/
│   ├── network/
│   ├── utils/
│   ├── metrics/
│   └── main.cpp
├── tests/
│   ├── unit/
│   ├── integration/
│   └── benchmarks/
├── config/
│   └── config.yaml
└── scripts/
    ├── build.sh
    └── deploy.sh
```

## Build Configuration

### Compiler Flags
```cmake
# Optimization flags
-O3 -march=native -mtune=native
-flto -fno-exceptions -fno-rtti

# Warning flags
-Wall -Wextra -Wpedantic -Werror

# Debug symbols (even in release for profiling)
-g -fno-omit-frame-pointer

# Link-time optimization
-flto=thin

# Security
-D_FORTIFY_SOURCE=2 -fstack-protector-strong
```

### Dependencies Installation
```bash
# Ubuntu/Debian
sudo apt-get install -y \
    build-essential \
    cmake \
    libboost-all-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libtbb-dev \
    libgoogle-perftools-dev

# Install simdjson
git clone https://github.com/simdjson/simdjson.git
cd simdjson && cmake . && make && sudo make install

# Install websocketpp
git clone https://github.com/zaphoyd/websocketpp.git
cd websocketpp && cmake . && make && sudo make install
```

## Testing Requirements

### Unit Tests
- Test each component in isolation
- Mock external dependencies
- Achieve >90% code coverage
- Use property-based testing for order book operations

### Integration Tests
- Test full data flow from exchange to API
- Simulate network failures and reconnections
- Test with recorded market data
- Verify order book consistency

### Performance Benchmarks
- Measure latency percentiles (p50, p95, p99, p99.9)
- Test throughput under various loads
- Profile memory usage and allocations
- Compare against baseline implementations

## Deployment Considerations

### Linux Kernel Tuning
```bash
# Network stack optimization
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.ipv4.tcp_rmem = 4096 87380 134217728
net.ipv4.tcp_wmem = 4096 65536 134217728

# CPU isolation for critical threads
isolcpus=2,3,4,5
```

### Process Configuration
- Pin threads to specific CPU cores
- Set real-time scheduling priority
- Disable CPU frequency scaling
- Use huge pages for memory allocation

## Monitoring and Observability

### Metrics to Export
- Message processing latency (histogram)
- Order book update rate (counter)
- WebSocket connection status (gauge)
- Memory pool utilization (gauge)
- CPU usage per thread (gauge)
- Network bandwidth usage (counter)

### Logging Strategy
- Async logging to avoid blocking
- Structured logging in JSON format
- Log rotation with size and time limits
- Different log levels for different components
- Correlation IDs for request tracing
