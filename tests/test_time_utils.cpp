/**
 * @file test_time_utils.cpp
 * @brief Unit tests for time utility functions
 */

#include <gtest/gtest.h>
#include "utils/time_utils.hpp"
#include <thread>
#include <chrono>

using namespace marketdata;

// ============================================================================
// Timestamp Tests
// ============================================================================

TEST(TimeUtilsTest, GetTimestampNs) {
    uint64_t ts1 = get_timestamp_ns();
    uint64_t ts2 = get_timestamp_ns();

    // Second timestamp should be >= first
    EXPECT_GE(ts2, ts1);

    // Should be reasonable (not zero, not too large)
    EXPECT_GT(ts1, 0);
}

TEST(TimeUtilsTest, TimestampMonotonic) {
    const int iterations = 1000;
    uint64_t prev = get_timestamp_ns();

    for (int i = 0; i < iterations; i++) {
        uint64_t current = get_timestamp_ns();
        EXPECT_GE(current, prev);
        prev = current;
    }
}

TEST(TimeUtilsTest, TimestampProgression) {
    uint64_t start = get_timestamp_ns();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    uint64_t end = get_timestamp_ns();
    uint64_t elapsed_ns = end - start;

    // Should have elapsed ~10ms = ~10,000,000 ns
    EXPECT_GT(elapsed_ns, 9'000'000);  // At least 9ms
    EXPECT_LT(elapsed_ns, 20'000'000); // Less than 20ms
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST(TimeUtilsTest, PerformanceGetTimestamp) {
    const int num_calls = 1000000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_calls; i++) {
        volatile uint64_t ts = get_timestamp_ns();
        (void)ts;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_calls;

    // Should be very fast (< 100 ns)
    EXPECT_LT(avg_ns, 100.0) << "Average get_timestamp_ns time: " << avg_ns << " ns";

    std::cout << "get_timestamp_ns avg: " << avg_ns << " ns\n";
}

// ============================================================================
// Concurrent Access
// ============================================================================

TEST(TimeUtilsTest, ConcurrentAccess) {
    const int num_threads = 8;
    const int calls_per_thread = 10000;

    std::vector<std::thread> threads;
    std::atomic<uint64_t> min_ts{UINT64_MAX};
    std::atomic<uint64_t> max_ts{0};

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([calls_per_thread, &min_ts, &max_ts]() {
            for (int i = 0; i < calls_per_thread; i++) {
                uint64_t ts = get_timestamp_ns();

                // Update min/max
                uint64_t current_min = min_ts.load();
                while (ts < current_min && !min_ts.compare_exchange_weak(current_min, ts));

                uint64_t current_max = max_ts.load();
                while (ts > current_max && !max_ts.compare_exchange_weak(current_max, ts));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All timestamps should be reasonable
    EXPECT_GT(min_ts.load(), 0);
    EXPECT_GT(max_ts.load(), min_ts.load());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
