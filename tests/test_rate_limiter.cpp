/**
 * @file test_rate_limiter.cpp
 * @brief Unit tests for RateLimiter (Token Bucket)
 */

#include <gtest/gtest.h>
#include "utils/rate_limiter.hpp"
#include <thread>
#include <vector>
#include <chrono>

using namespace marketdata;

class RateLimiterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a rate limiter: 10 tokens/second, capacity 10
        limiter = std::make_unique<RateLimiter>(10.0, 10);
    }

    std::unique_ptr<RateLimiter> limiter;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(RateLimiterTest, InitialState) {
    // Should have full capacity initially
    EXPECT_TRUE(limiter->try_acquire(1));
}

TEST_F(RateLimiterTest, SingleAcquire) {
    EXPECT_TRUE(limiter->try_acquire(1));
    EXPECT_TRUE(limiter->try_acquire(1));
    EXPECT_TRUE(limiter->try_acquire(1));
}

TEST_F(RateLimiterTest, AcquireMultipleTokens) {
    EXPECT_TRUE(limiter->try_acquire(5));  // 5 tokens
    EXPECT_TRUE(limiter->try_acquire(3));  // 3 more
    EXPECT_TRUE(limiter->try_acquire(2));  // 2 more = 10 total
}

TEST_F(RateLimiterTest, ExhaustCapacity) {
    // Exhaust all 10 tokens
    EXPECT_TRUE(limiter->try_acquire(10));

    // Next acquire should fail
    EXPECT_FALSE(limiter->try_acquire(1));
}

TEST_F(RateLimiterTest, PartialExhaust) {
    EXPECT_TRUE(limiter->try_acquire(8));
    EXPECT_TRUE(limiter->try_acquire(2));  // Total: 10

    // Should fail
    EXPECT_FALSE(limiter->try_acquire(1));
}

// ============================================================================
// Token Refill
// ============================================================================

TEST_F(RateLimiterTest, TokenRefill) {
    // Exhaust tokens
    limiter->try_acquire(10);
    EXPECT_FALSE(limiter->try_acquire(1));

    // Wait for refill (10 tokens/sec = 1 token per 100ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Should have at least 1 token now
    EXPECT_TRUE(limiter->try_acquire(1));
}

TEST_F(RateLimiterTest, GradualRefill) {
    // Exhaust tokens
    limiter->try_acquire(10);

    // Wait for 500ms (should refill ~5 tokens at 10/sec)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Should be able to acquire ~5 tokens
    EXPECT_TRUE(limiter->try_acquire(4));  // Conservative estimate
}

TEST_F(RateLimiterTest, RefillCapping) {
    // Wait a long time
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Should not exceed capacity (10 tokens)
    EXPECT_TRUE(limiter->try_acquire(10));
    EXPECT_FALSE(limiter->try_acquire(1));  // Should fail
}

// ============================================================================
// Blocking Acquire
// ============================================================================

TEST_F(RateLimiterTest, BlockingAcquireImmediate) {
    auto start = std::chrono::steady_clock::now();

    limiter->acquire(1);  // Should succeed immediately

    auto duration = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

    // Should be very fast (< 10ms)
    EXPECT_LT(ms, 10);
}

TEST_F(RateLimiterTest, BlockingAcquireWaits) {
    // Exhaust tokens
    limiter->try_acquire(10);

    auto start = std::chrono::steady_clock::now();

    // This should block until tokens are available
    limiter->acquire(1);

    auto duration = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

    // Should have waited ~100ms (1 token at 10/sec)
    EXPECT_GE(ms, 80);   // Allow some tolerance
    EXPECT_LT(ms, 200);  // But not too long
}

// ============================================================================
// Different Rates
// ============================================================================

TEST(RateLimiterCustomTest, HighRate) {
    // 100 tokens/second
    RateLimiter limiter(100.0, 100);

    EXPECT_TRUE(limiter.try_acquire(100));
    EXPECT_FALSE(limiter.try_acquire(1));

    // Wait 100ms (should refill ~10 tokens)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_TRUE(limiter.try_acquire(8));  // Conservative
}

TEST(RateLimiterCustomTest, LowRate) {
    // 1 token/second
    RateLimiter limiter(1.0, 5);

    EXPECT_TRUE(limiter.try_acquire(5));
    EXPECT_FALSE(limiter.try_acquire(1));

    // Wait 1 second (should refill 1 token)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_TRUE(limiter.try_acquire(1));
}

TEST(RateLimiterCustomTest, FractionalRate) {
    // 2.5 tokens/second
    RateLimiter limiter(2.5, 10);

    EXPECT_TRUE(limiter.try_acquire(10));

    // Wait 800ms (should refill 2.5 * 0.8 = 2 tokens)
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    EXPECT_TRUE(limiter.try_acquire(2));
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(RateLimiterTest, ZeroTokensRequest) {
    EXPECT_TRUE(limiter->try_acquire(0));  // Should succeed immediately
}

TEST_F(RateLimiterTest, RequestMoreThanCapacity) {
    // Request more than capacity
    EXPECT_FALSE(limiter->try_acquire(20));  // Capacity is 10
}

TEST_F(RateLimiterTest, MultipleSmallAcquires) {
    // Acquire one at a time until exhausted
    int count = 0;
    while (limiter->try_acquire(1)) {
        count++;
        if (count > 20) break;  // Safety limit
    }

    EXPECT_EQ(count, 10);  // Should have acquired exactly 10
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST_F(RateLimiterTest, ConcurrentTryAcquire) {
    const int num_threads = 8;
    const int attempts_per_thread = 100;

    std::atomic<int> successful_acquires{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, attempts_per_thread, &successful_acquires]() {
            for (int i = 0; i < attempts_per_thread; i++) {
                if (limiter->try_acquire(1)) {
                    successful_acquires++;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Should have acquired many tokens (with refill during execution)
    EXPECT_GT(successful_acquires.load(), 10);  // At least initial capacity
}

TEST_F(RateLimiterTest, ConcurrentBlockingAcquire) {
    const int num_threads = 4;
    const int acquires_per_thread = 5;

    std::atomic<int> total_acquires{0};
    std::vector<std::thread> threads;

    auto start = std::chrono::steady_clock::now();

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, acquires_per_thread, &total_acquires]() {
            for (int i = 0; i < acquires_per_thread; i++) {
                limiter->acquire(1);  // Blocking
                total_acquires++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto duration = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

    // Should have acquired exactly 20 tokens
    EXPECT_EQ(total_acquires.load(), num_threads * acquires_per_thread);

    // With 10 tokens/sec, 20 tokens should take ~1-2 seconds
    std::cout << "Acquired " << total_acquires.load() << " tokens in " << ms << " ms\n";
}

// ============================================================================
// Real-World Scenarios
// ============================================================================

TEST(RateLimiterScenarioTest, BinanceAPILimit) {
    // Binance: 1200 requests/minute = 20 req/sec
    RateLimiter binance_limiter(20.0, 20);

    // Burst of requests
    int successful = 0;
    for (int i = 0; i < 25; i++) {
        if (binance_limiter.try_acquire(1)) {
            successful++;
        }
    }

    // Should allow ~20 requests
    EXPECT_EQ(successful, 20);

    // Wait 1 second
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Should have ~20 more tokens
    successful = 0;
    for (int i = 0; i < 25; i++) {
        if (binance_limiter.try_acquire(1)) {
            successful++;
        }
    }

    EXPECT_GE(successful, 18);  // Allow some tolerance
}

TEST(RateLimiterScenarioTest, CoinbaseAPILimit) {
    // Coinbase Pro: 10 req/sec
    RateLimiter coinbase_limiter(10.0, 10);

    // Try to send 15 requests immediately
    int successful = 0;
    for (int i = 0; i < 15; i++) {
        if (coinbase_limiter.try_acquire(1)) {
            successful++;
        }
    }

    // Should only allow 10
    EXPECT_EQ(successful, 10);
}

TEST(RateLimiterScenarioTest, BurstThenSteady) {
    RateLimiter limiter(10.0, 100);  // High capacity

    // Burst: use 50 tokens
    EXPECT_TRUE(limiter.try_acquire(50));

    // Steady stream: 1 token every 200ms (5/sec < 10/sec rate)
    int successful = 0;
    for (int i = 0; i < 20; i++) {
        if (limiter.try_acquire(1)) {
            successful++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Should succeed for all (refill rate > consumption rate)
    EXPECT_EQ(successful, 20);
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(RateLimiterTest, PerformanceTryAcquire) {
    const int num_attempts = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_attempts; i++) {
        limiter->try_acquire(1);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_attempts;

    // Should be very fast (< 100 ns)
    EXPECT_LT(avg_ns, 100.0) << "Average try_acquire time: " << avg_ns << " ns";

    std::cout << "RateLimiter try_acquire avg: " << avg_ns << " ns\n";
}

// ============================================================================
// Configuration Tests
// ============================================================================

TEST(RateLimiterConfigTest, DifferentCapacities) {
    RateLimiter small(10.0, 5);
    RateLimiter large(10.0, 100);

    // Small capacity
    EXPECT_TRUE(small.try_acquire(5));
    EXPECT_FALSE(small.try_acquire(1));

    // Large capacity
    EXPECT_TRUE(large.try_acquire(100));
    EXPECT_FALSE(large.try_acquire(1));
}

TEST(RateLimiterConfigTest, RateVsCapacity) {
    // High rate, low capacity
    RateLimiter fast_small(100.0, 10);

    // Low rate, high capacity
    RateLimiter slow_large(1.0, 100);

    // Fast refill
    fast_small.try_acquire(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(fast_small.try_acquire(10));  // Refilled quickly

    // Slow refill
    slow_large.try_acquire(100);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(slow_large.try_acquire(10));  // Still empty
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST_F(RateLimiterTest, RapidFireTryAcquire) {
    int successful = 0;
    int failed = 0;

    // Rapidly try to acquire
    for (int i = 0; i < 1000; i++) {
        if (limiter->try_acquire(1)) {
            successful++;
        } else {
            failed++;
        }
    }

    EXPECT_GT(successful, 0);
    EXPECT_GT(failed, 0);  // Should have some failures

    std::cout << "Rapid fire: " << successful << " succeeded, " << failed << " failed\n";
}

TEST_F(RateLimiterTest, LongRunningStress) {
    // Run for 2 seconds, trying to acquire every 50ms
    auto start = std::chrono::steady_clock::now();
    int attempts = 0;
    int successful = 0;

    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        attempts++;
        if (limiter->try_acquire(1)) {
            successful++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // At 10 tokens/sec for 2 seconds = ~20 tokens
    // With attempts every 50ms = 40 attempts
    // Success rate should be ~50%

    std::cout << "Long running: " << successful << " / " << attempts << " succeeded\n";

    EXPECT_GT(successful, 15);  // At least 15 tokens
    EXPECT_LT(successful, 25);  // Not more than 25
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
