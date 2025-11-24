/**
 * @file test_rate_limiter.cpp
 * @brief Unit tests for rate limiter (token bucket)
 */

#include <gtest/gtest.h>
#include "utils/rate_limiter.hpp"
#include <thread>
#include <chrono>
#include <vector>

using namespace marketdata;

/**
 * @brief Test fixture for rate limiter tests
 */
class RateLimiterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Nothing to set up
    }

    void TearDown() override {
        // Nothing to tear down
    }
};

/**
 * @brief Test basic token acquisition
 */
TEST_F(RateLimiterTest, BasicAcquisition) {
    RateLimiter limiter(10.0, 10);  // 10 tokens/sec, capacity 10

    // Should have 10 tokens initially
    EXPECT_DOUBLE_EQ(limiter.get_current_tokens(), 10.0);

    // Acquire 5 tokens
    ASSERT_TRUE(limiter.try_acquire(5));
    EXPECT_NEAR(limiter.get_current_tokens(), 5.0, 0.1);

    // Acquire 5 more
    ASSERT_TRUE(limiter.try_acquire(5));
    EXPECT_NEAR(limiter.get_current_tokens(), 0.0, 0.1);

    // Try to acquire when depleted
    ASSERT_FALSE(limiter.try_acquire());
}

/**
 * @brief Test token refill
 */
TEST_F(RateLimiterTest, TokenRefill) {
    RateLimiter limiter(10.0, 10);  // 10 tokens/sec

    // Deplete tokens
    ASSERT_TRUE(limiter.try_acquire(10));
    EXPECT_NEAR(limiter.get_current_tokens(), 0.0, 0.1);

    // Wait for 100ms (should refill ~1 token)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Should have ~1 token now
    double tokens = limiter.get_current_tokens();
    EXPECT_GE(tokens, 0.8);
    EXPECT_LE(tokens, 1.2);

    // Should be able to acquire 1 token
    ASSERT_TRUE(limiter.try_acquire());
}

/**
 * @brief Test capacity limit
 */
TEST_F(RateLimiterTest, CapacityLimit) {
    RateLimiter limiter(100.0, 10);  // High rate, but capacity 10

    // Wait for a while (tokens shouldn't exceed capacity)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Should still have max 10 tokens
    double tokens = limiter.get_current_tokens();
    EXPECT_LE(tokens, 10.0);
    EXPECT_GE(tokens, 9.5);  // Allow small measurement error
}

/**
 * @brief Test blocking acquisition
 */
TEST_F(RateLimiterTest, BlockingAcquisition) {
    RateLimiter limiter(10.0, 10);

    // Deplete all tokens
    ASSERT_TRUE(limiter.try_acquire(10));

    auto start = std::chrono::steady_clock::now();

    // This should block for ~100ms (until 1 token refills)
    limiter.acquire();

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // Should have waited at least 50ms (conservative check)
    EXPECT_GE(elapsed_ms, 50);

    // Should have waited less than 300ms (generous upper bound)
    EXPECT_LE(elapsed_ms, 300);
}

/**
 * @brief Test try_acquire_for with timeout
 */
TEST_F(RateLimiterTest, TryAcquireWithTimeout) {
    RateLimiter limiter(10.0, 10);

    // Deplete all tokens
    ASSERT_TRUE(limiter.try_acquire(10));

    // Try to acquire with short timeout (should fail)
    auto start = std::chrono::steady_clock::now();
    ASSERT_FALSE(limiter.try_acquire_for(50));  // 50ms timeout
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // Should have waited approximately the timeout
    EXPECT_GE(elapsed_ms, 40);
    EXPECT_LE(elapsed_ms, 100);

    // Try with longer timeout (should succeed)
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(limiter.try_acquire_for(150));  // 150ms timeout
    elapsed = std::chrono::steady_clock::now() - start;
    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // Should have succeeded before timeout
    EXPECT_LE(elapsed_ms, 150);
}

/**
 * @brief Test reset functionality
 */
TEST_F(RateLimiterTest, Reset) {
    RateLimiter limiter(10.0, 10);

    // Deplete tokens
    ASSERT_TRUE(limiter.try_acquire(10));
    EXPECT_NEAR(limiter.get_current_tokens(), 0.0, 0.1);

    // Reset
    limiter.reset();

    // Should have full capacity again
    EXPECT_NEAR(limiter.get_current_tokens(), 10.0, 0.1);
    ASSERT_TRUE(limiter.try_acquire(10));
}

/**
 * @brief Test concurrent access
 */
TEST_F(RateLimiterTest, ConcurrentAccess) {
    RateLimiter limiter(100.0, 100);  // High rate for this test

    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};

    // Create multiple threads trying to acquire
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 20; j++) {
                if (limiter.try_acquire()) {
                    success_count.fetch_add(1);
                } else {
                    failure_count.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    // Join all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Total attempts = 10 threads * 20 attempts = 200
    int total = success_count.load() + failure_count.load();
    EXPECT_EQ(total, 200);

    // Should have some successes (initial capacity + refills)
    EXPECT_GT(success_count.load(), 100);

    // Should have some failures (rate limiting kicked in)
    EXPECT_GT(failure_count.load(), 0);
}

/**
 * @brief Test rate change
 */
TEST_F(RateLimiterTest, RateChange) {
    RateLimiter limiter(10.0, 10);

    // Deplete tokens
    ASSERT_TRUE(limiter.try_acquire(10));

    // Change to higher rate
    limiter.set_rate(100.0);

    // Wait 100ms (should refill ~10 tokens at new rate)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    double tokens = limiter.get_current_tokens();
    EXPECT_GE(tokens, 8.0);  // Should have refilled significantly
}

/**
 * @brief Test capacity change
 */
TEST_F(RateLimiterTest, CapacityChange) {
    RateLimiter limiter(10.0, 10);

    // Start with 10 tokens
    EXPECT_NEAR(limiter.get_current_tokens(), 10.0, 0.1);

    // Reduce capacity to 5
    limiter.set_capacity(5);

    // Tokens should be capped to new capacity
    double tokens = limiter.get_current_tokens();
    EXPECT_LE(tokens, 5.0);

    // Should not be able to acquire more than capacity
    ASSERT_TRUE(limiter.try_acquire(5));
    ASSERT_FALSE(limiter.try_acquire());
}

/**
 * @brief Test ExchangeRateLimiters
 */
TEST_F(RateLimiterTest, ExchangeRateLimiters) {
    ExchangeRateLimiters limiters;

    // Should be able to acquire from each exchange
    ASSERT_TRUE(limiters.try_acquire(Exchange::BINANCE));
    ASSERT_TRUE(limiters.try_acquire(Exchange::COINBASE));
    ASSERT_TRUE(limiters.try_acquire(Exchange::KRAKEN));

    // Get limiters for each exchange
    auto* binance = limiters.get_limiter(Exchange::BINANCE);
    auto* coinbase = limiters.get_limiter(Exchange::COINBASE);
    auto* kraken = limiters.get_limiter(Exchange::KRAKEN);

    ASSERT_NE(binance, nullptr);
    ASSERT_NE(coinbase, nullptr);
    ASSERT_NE(kraken, nullptr);

    // Verify different rates
    EXPECT_DOUBLE_EQ(binance->get_rate(), 20.0);
    EXPECT_DOUBLE_EQ(coinbase->get_rate(), 30.0);
    EXPECT_DOUBLE_EQ(kraken->get_rate(), 15.0);
}

/**
 * @brief Test burst capacity
 */
TEST_F(RateLimiterTest, BurstCapacity) {
    RateLimiter limiter(10.0, 20);  // 10/sec rate, 20 burst capacity

    // Should be able to acquire 20 immediately (burst)
    for (int i = 0; i < 20; i++) {
        ASSERT_TRUE(limiter.try_acquire()) << "Failed at token " << i;
    }

    // 21st should fail (burst depleted)
    ASSERT_FALSE(limiter.try_acquire());

    // Wait for 1 second (should refill 10 tokens)
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Should be able to acquire ~10 more
    int acquired = 0;
    for (int i = 0; i < 15; i++) {
        if (limiter.try_acquire()) {
            acquired++;
        }
    }

    EXPECT_GE(acquired, 9);   // At least 9 (accounting for timing)
    EXPECT_LE(acquired, 11);  // At most 11 (accounting for timing)
}

/**
 * @brief Test zero rate (should not hang)
 */
TEST_F(RateLimiterTest, ZeroRate) {
    // Constructor should handle zero rate gracefully
    RateLimiter limiter(0.0, 10);

    // Should have been adjusted to minimum rate
    EXPECT_GT(limiter.get_rate(), 0.0);

    // Should still be able to use initial capacity
    ASSERT_TRUE(limiter.try_acquire(10));
}

/**
 * @brief Test multiple token acquisition
 */
TEST_F(RateLimiterTest, MultipleTokens) {
    RateLimiter limiter(10.0, 20);

    // Acquire 5 tokens at once
    ASSERT_TRUE(limiter.try_acquire(5));
    EXPECT_NEAR(limiter.get_current_tokens(), 15.0, 0.1);

    // Acquire 10 more
    ASSERT_TRUE(limiter.try_acquire(10));
    EXPECT_NEAR(limiter.get_current_tokens(), 5.0, 0.1);

    // Try to acquire 10 (should fail, only 5 available)
    ASSERT_FALSE(limiter.try_acquire(10));

    // But acquiring 5 should work
    ASSERT_TRUE(limiter.try_acquire(5));
}

/**
 * @brief Main function
 */
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
