#include <iostream>
#include <iomanip>
#include "core/order_book.hpp"
#include "core/order_book_manager.hpp"
#include "metrics/metrics_collector.hpp"
#include "utils/time_utils.hpp"

using namespace marketdata;

void print_separator() {
    std::cout << std::string(80, '=') << '\n';
}

void print_snapshot(const OrderBookSnapshot& snapshot) {
    std::cout << "\nOrder Book Snapshot for " << snapshot.symbol << '\n';
    std::cout << "Sequence: " << snapshot.sequence_number << '\n';
    std::cout << "Timestamp: " << snapshot.timestamp << " ns\n\n";

    std::cout << std::left << std::setw(15) << "BIDS"
              << std::setw(15) << "Price"
              << std::setw(15) << "Quantity"
              << std::setw(15) << "Orders" << '\n';
    print_separator();

    size_t max_rows = std::max(snapshot.bids.size(), snapshot.asks.size());

    for (size_t i = 0; i < max_rows; ++i) {
        // Print bid
        if (i < snapshot.bids.size()) {
            std::cout << std::setw(15) << " ";
            std::cout << std::setw(15) << std::fixed << std::setprecision(2)
                      << snapshot.bids[i].price;
            std::cout << std::setw(15) << std::fixed << std::setprecision(4)
                      << snapshot.bids[i].quantity;
            std::cout << std::setw(15) << snapshot.bids[i].order_count;
        } else {
            std::cout << std::setw(60) << " ";
        }

        std::cout << " | ";

        // Print ask
        if (i < snapshot.asks.size()) {
            std::cout << std::setw(15) << std::fixed << std::setprecision(2)
                      << snapshot.asks[i].price;
            std::cout << std::setw(15) << std::fixed << std::setprecision(4)
                      << snapshot.asks[i].quantity;
            std::cout << std::setw(15) << snapshot.asks[i].order_count;
            std::cout << std::setw(15) << "ASKS";
        }

        std::cout << '\n';
    }

    print_separator();
}

void demonstrate_order_book() {
    std::cout << "\n=== Low-Latency Order Book Demo ===\n\n";

    // Create order book
    OrderBook<1000> book;

    std::cout << "1. Adding initial orders...\n";

    // Add some bids
    book.update_bid(99.50, 100.0);
    book.update_bid(99.25, 150.0);
    book.update_bid(99.00, 200.0);
    book.update_bid(98.75, 250.0);

    // Add some asks
    book.update_ask(100.00, 120.0);
    book.update_ask(100.25, 180.0);
    book.update_ask(100.50, 220.0);
    book.update_ask(100.75, 280.0);

    // Get and print snapshot
    auto snapshot = book.get_snapshot(10);
    snapshot.symbol = "BTC-USD";
    print_snapshot(snapshot);

    // Print statistics
    std::cout << "\n2. Order Book Statistics:\n";
    print_separator();

    double spread = book.get_spread();
    double mid_price = book.get_mid_price();
    double imbalance = book.get_book_imbalance();
    auto stats = book.get_statistics();

    std::cout << "Spread:         " << std::fixed << std::setprecision(2)
              << spread << " USD\n";
    std::cout << "Mid Price:      " << std::fixed << std::setprecision(2)
              << mid_price << " USD\n";
    std::cout << "Book Imbalance: " << std::fixed << std::setprecision(4)
              << imbalance << "\n";
    std::cout << "Bid Depth:      " << stats.bid_depth << " levels\n";
    std::cout << "Ask Depth:      " << stats.ask_depth << " levels\n";
    std::cout << "Total Bid Vol:  " << std::fixed << std::setprecision(4)
              << fixed_to_double(stats.total_bid_volume) << "\n";
    std::cout << "Total Ask Vol:  " << std::fixed << std::setprecision(4)
              << fixed_to_double(stats.total_ask_volume) << "\n";

    print_separator();

    // Update an existing level
    std::cout << "\n3. Updating existing bid level at 99.50...\n";
    book.update_bid(99.50, 250.0);  // Increase quantity

    snapshot = book.get_snapshot(10);
    snapshot.symbol = "BTC-USD";
    print_snapshot(snapshot);

    // Remove a level
    std::cout << "\n4. Removing bid level at 98.75...\n";
    book.update_bid(98.75, 0.0);  // Remove by setting quantity to 0

    snapshot = book.get_snapshot(10);
    snapshot.symbol = "BTC-USD";
    print_snapshot(snapshot);
}

void demonstrate_metrics() {
    std::cout << "\n=== Metrics Collection Demo ===\n\n";

    MetricsCollector metrics;
    OrderBook<1000> book;

    std::cout << "Performing 10,000 order book updates...\n";

    auto start_time = get_timestamp_ns();

    for (int i = 0; i < 10000; ++i) {
        auto update_start = get_timestamp_ns();

        // Alternate between bids and asks
        if (i % 2 == 0) {
            book.update_bid(99.0 + (i % 100) * 0.01, 100.0 + i * 0.1);
        } else {
            book.update_ask(100.0 + (i % 100) * 0.01, 100.0 + i * 0.1);
        }

        auto update_end = get_timestamp_ns();
        metrics.record_update_latency_ns(update_end - update_start);
        metrics.increment_updates();
    }

    auto end_time = get_timestamp_ns();
    uint64_t total_time_us = (end_time - start_time) / 1000;

    print_separator();
    std::cout << "\nPerformance Metrics:\n";
    print_separator();

    std::cout << "Total Updates:      10,000\n";
    std::cout << "Total Time:         " << total_time_us << " μs\n";
    std::cout << "Throughput:         "
              << (10000.0 / (total_time_us / 1000000.0)) << " updates/sec\n\n";

    std::cout << "Latency Percentiles (nanoseconds):\n";
    std::cout << "  p50 (median):     " << metrics.get_update_latency_p50() << " ns\n";
    std::cout << "  p95:              " << metrics.get_update_latency_p95() << " ns\n";
    std::cout << "  p99:              " << metrics.get_update_latency_p99() << " ns\n";
    std::cout << "  p99.9:            " << metrics.get_update_latency_p999() << " ns\n";

    print_separator();
}

void demonstrate_order_book_manager() {
    std::cout << "\n=== Order Book Manager Demo ===\n\n";

    OrderBookManager manager;

    // Create order books for multiple symbols
    std::cout << "Creating order books for multiple symbols...\n";
    manager.create_order_book("BTC-USD");
    manager.create_order_book("ETH-USD");
    manager.create_order_book("SOL-USD");

    // Process some updates
    NormalizedOrderBookUpdate btc_update;
    btc_update.exchange = Exchange::BINANCE;
    btc_update.symbol = "BTC-USD";
    btc_update.timestamp = get_timestamp_ns();
    btc_update.bids.emplace_back(99.50, 100.0);
    btc_update.bids.emplace_back(99.25, 150.0);
    btc_update.asks.emplace_back(100.00, 120.0);
    btc_update.asks.emplace_back(100.25, 180.0);

    manager.process_update(btc_update);

    NormalizedOrderBookUpdate eth_update;
    eth_update.exchange = Exchange::COINBASE;
    eth_update.symbol = "ETH-USD";
    eth_update.timestamp = get_timestamp_ns();
    eth_update.bids.emplace_back(3.50, 1000.0);
    eth_update.bids.emplace_back(3.45, 1500.0);
    eth_update.asks.emplace_back(3.55, 1200.0);
    eth_update.asks.emplace_back(3.60, 1800.0);

    manager.process_update(eth_update);

    // Print snapshots
    print_separator();
    std::cout << "\nManaged Order Books:\n";

    for (const auto& symbol : manager.get_symbols()) {
        auto snapshot = manager.get_snapshot(symbol, 5);
        if (snapshot) {
            std::cout << "\n" << symbol << ":\n";
            std::cout << "  Best Bid: " << std::fixed << std::setprecision(2)
                      << (snapshot->bids.empty() ? 0.0 : snapshot->bids[0].price) << "\n";
            std::cout << "  Best Ask: " << std::fixed << std::setprecision(2)
                      << (snapshot->asks.empty() ? 0.0 : snapshot->asks[0].price) << "\n";

            auto mid_price = manager.get_mid_price(symbol);
            if (mid_price) {
                std::cout << "  Mid Price: " << std::fixed << std::setprecision(2)
                          << *mid_price << "\n";
            }
        }
    }

    print_separator();
}

int main() {
    try {
        std::cout << "\n";
        print_separator();
        std::cout << "  CRYPTO ORDER BOOK ENGINE - CORE DEMONSTRATION\n";
        print_separator();

        demonstrate_order_book();
        demonstrate_metrics();
        demonstrate_order_book_manager();

        std::cout << "\n\nCore order book system demonstration complete!\n";
        std::cout << "\nNext steps:\n";
        std::cout << "  - Implement exchange connectors (Binance, Coinbase, Kraken)\n";
        std::cout << "  - Implement WebSocket server for client connections\n";
        std::cout << "  - Implement REST API endpoints\n";
        std::cout << "  - Add SIMD optimizations\n";
        std::cout << "  - Deploy with real-time market data feeds\n\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
