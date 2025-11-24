/**
 * @file test_metrics_collector.cpp
 * @brief Unit tests for MetricsCollector
 */

#include <gtest/gtest.h>
#include "metrics/metrics_collector.hpp"
#include <thread>

using namespace marketdata;

class MetricsCollectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        collector = std::make_unique<MetricsCollector>();
    }

    std::unique_ptr<MetricsCollector> collector;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(MetricsCollectorTest, InitialState) {
    auto metrics = collector->get_metrics();

    EXPECT_EQ(metrics.total_updates, 0);
    EXPECT_DOUBLE_EQ(metrics.updates_per_second, 0.0);
}

TEST_F(MetricsCollectorTest, RecordUpdate) {
    collector->record_update("BTCUSD", Exchange::BINANCE);

    auto metrics = collector->get_metrics();
    EXPECT_EQ(metrics.total_updates, 1);
}

TEST_F(MetricsCollectorTest, RecordMultipleUpdates) {
    for (int i = 0; i < 10; i++) {
        collector->record_update("BTCUSD", Exchange::BINANCE);
    }

    auto metrics = collector->get_metrics();
    EXPECT_EQ(metrics.total_updates, 10);
}

// ============================================================================
// Latency Metrics
// ============================================================================

TEST_F(MetricsCollectorTest, RecordLatency) {
    collector->record_latency(1000);  // 1000 ns

    auto latency_stats = collector->get_latency_stats();

    EXPECT_GT(latency_stats.count, 0);
    EXPECT_GT(latency_stats.average_ns, 0.0);
}

TEST_F(MetricsCollectorTest, LatencyStatistics) {
    // Record varying latencies
    collector->record_latency(100);
    collector->record_latency(200);
    collector->record_latency(300);
    collector->record_latency(400);
    collector->record_latency(500);

    auto stats = collector->get_latency_stats();

    EXPECT_EQ(stats.count, 5);
    EXPECT_DOUBLE_EQ(stats.average_ns, 300.0);  // (100+200+300+400+500)/5
    EXPECT_DOUBLE_EQ(stats.min_ns, 100.0);
    EXPECT_DOUBLE_EQ(stats.max_ns, 500.0);
}

// ============================================================================
// Per-Symbol Metrics
// ============================================================================

TEST_F(MetricsCollectorTest, SymbolMetrics) {
    collector->record_update("BTCUSD", Exchange::BINANCE);
    collector->record_update("BTCUSD", Exchange::COINBASE);
    collector->record_update("ETHUSD", Exchange::BINANCE);

    auto symbol_metrics = collector->get_symbol_metrics("BTCUSD");

    EXPECT_EQ(symbol_metrics.update_count, 2);
}

TEST_F(MetricsCollectorTest, MultipleSymbols) {
    collector->record_update("BTCUSD", Exchange::BINANCE);
    collector->record_update("ETHUSD", Exchange::BINANCE);
    collector->record_update("BNBUSD", Exchange::BINANCE);

    auto all_symbols = collector->get_all_symbols();

    EXPECT_EQ(all_symbols.size(), 3);
}

// ============================================================================
// Reset Operations
// ============================================================================

TEST_F(MetricsCollectorTest, Reset) {
    collector->record_update("BTCUSD", Exchange::BINANCE);
    collector->record_latency(1000);

    collector->reset();

    auto metrics = collector->get_metrics();
    EXPECT_EQ(metrics.total_updates, 0);
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(MetricsCollectorTest, PerformanceRecordUpdate) {
    const int num_records = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_records; i++) {
        collector->record_update("BTCUSD", Exchange::BINANCE);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_records;

    EXPECT_LT(avg_ns, 500.0) << "Average record time: " << avg_ns << " ns";

    std::cout << "MetricsCollector record avg: " << avg_ns << " ns\n";
}

// ============================================================================
// Concurrent Access
// ============================================================================

TEST_F(MetricsCollectorTest, ConcurrentRecording) {
    const int num_threads = 8;
    const int records_per_thread = 1000;

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, records_per_thread]() {
            for (int i = 0; i < records_per_thread; i++) {
                collector->record_update("BTCUSD", Exchange::BINANCE);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto metrics = collector->get_metrics();
    EXPECT_EQ(metrics.total_updates, num_threads * records_per_thread);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
