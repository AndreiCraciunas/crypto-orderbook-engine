/**
 * @file rate_limiter.hpp
 * @brief Token bucket rate limiter for API requests
 *
 * Implements token bucket algorithm for rate limiting API requests.
 * Provides both blocking and non-blocking modes with configurable
 * rates and burst capacity.
 *
 * **Token Bucket Algorithm:**
 * - Tokens added at constant rate (tokens/second)
 * - Each request consumes one token
 * - Burst capacity allows temporary spikes
 * - Blocks or rejects when tokens depleted
 *
 * **Performance:**
 * - Acquire: O(1), ~50-100 ns
 * - Thread-safe via atomic operations
 * - Lock-free in fast path
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <atomic>
#include <chrono>
#include <thread>

namespace marketdata {

/**
 * @brief Token bucket rate limiter
 *
 * Thread-safe rate limiter using token bucket algorithm. Allows bursts
 * up to capacity, then throttles to configured rate.
 *
 * **Algorithm:**
 * 1. Tokens refill at constant rate (tokens per second)
 * 2. Bucket capacity limits maximum tokens
 * 3. Each acquire() consumes one token
 * 4. If no tokens available:
 *    - Blocking mode: Wait for token
 *    - Non-blocking mode: Return false
 *
 * **Example Rates:**
 * - Binance: 1200 req/min = 20 req/sec
 * - Coinbase: No explicit limit (~30 req/sec recommended)
 * - Kraken: 1 req/sec for private, unlimited for public
 *
 * @code
 * // Create rate limiter: 10 requests/sec, burst of 20
 * RateLimiter limiter(10.0, 20);
 *
 * // Blocking acquire (waits if needed)
 * limiter.acquire();  // Blocks until token available
 * send_request();
 *
 * // Non-blocking acquire
 * if (limiter.try_acquire()) {
 *     send_request();
 * } else {
 *     std::cout << "Rate limit reached, try later\n";
 * }
 *
 * // Acquire multiple tokens
 * if (limiter.try_acquire(5)) {  // Need 5 tokens
 *     send_batch_request(5);
 * }
 * @endcode
 *
 * **Thread Safety:**
 * - All methods are thread-safe
 * - Uses atomic operations (lock-free)
 * - Safe for concurrent access
 *
 * **Burst Handling:**
 * - Capacity allows temporary bursts
 * - Example: 10 req/sec, capacity 20
 *   - Can send 20 requests immediately
 *   - Then throttled to 10 req/sec
 *   - Refills at 10 tokens/sec
 *
 * @note Thread-safe, lock-free
 * @note Precision limited by system clock (typically 1ms)
 */
class RateLimiter {
public:
    /**
     * @brief Construct rate limiter
     *
     * @param rate_per_second Token refill rate (tokens per second)
     * @param capacity Bucket capacity (max tokens)
     *
     * @code
     * RateLimiter limiter(10.0, 20);  // 10/sec, burst 20
     * @endcode
     *
     * @note Capacity should be >= 1
     * @note Rate should be > 0
     */
    explicit RateLimiter(double rate_per_second, size_t capacity);

    /**
     * @brief Acquire token (blocking)
     *
     * Blocks until a token becomes available. Guaranteed to succeed.
     *
     * @param tokens Number of tokens to acquire (default: 1)
     *
     * @code
     * limiter.acquire();     // Wait for 1 token
     * limiter.acquire(5);    // Wait for 5 tokens
     * @endcode
     *
     * @note Thread-safe
     * @note May block indefinitely if rate is 0
     * @warning Do not call from time-critical threads
     */
    void acquire(size_t tokens = 1);

    /**
     * @brief Try to acquire token (non-blocking)
     *
     * Attempts to acquire token without blocking.
     *
     * @param tokens Number of tokens to acquire (default: 1)
     * @return bool True if tokens acquired, false otherwise
     *
     * @code
     * if (limiter.try_acquire()) {
     *     // Token acquired, safe to proceed
     *     send_request();
     * } else {
     *     // Rate limit reached
     *     std::cout << "Rate limited\n";
     * }
     * @endcode
     *
     * @note Thread-safe
     * @note Returns immediately (O(1))
     */
    bool try_acquire(size_t tokens = 1);

    /**
     * @brief Try to acquire with timeout
     *
     * Attempts to acquire token, waiting up to timeout.
     *
     * @param timeout_ms Timeout in milliseconds
     * @param tokens Number of tokens to acquire (default: 1)
     * @return bool True if tokens acquired, false if timeout
     *
     * @code
     * if (limiter.try_acquire_for(1000)) {  // Wait up to 1 second
     *     send_request();
     * } else {
     *     std::cout << "Timeout waiting for token\n";
     * }
     * @endcode
     *
     * @note Thread-safe
     * @note May return early if tokens become available
     */
    bool try_acquire_for(uint64_t timeout_ms, size_t tokens = 1);

    /**
     * @brief Reset rate limiter
     *
     * Resets token count to full capacity.
     *
     * @code
     * limiter.reset();  // Refill to capacity
     * @endcode
     *
     * @note Thread-safe
     * @note Useful for testing or after long idle periods
     */
    void reset();

    /**
     * @brief Get current token count
     *
     * Returns approximate current token count. May be stale
     * due to concurrent access.
     *
     * @return double Current tokens (approximate)
     *
     * @note Thread-safe
     * @note Result may be outdated immediately
     * @note Useful for monitoring/debugging
     */
    double get_current_tokens() const;

    /**
     * @brief Get configured rate
     *
     * @return double Tokens per second
     *
     * @note Thread-safe
     */
    double get_rate() const { return rate_per_second_; }

    /**
     * @brief Get bucket capacity
     *
     * @return size_t Max tokens
     *
     * @note Thread-safe
     */
    size_t get_capacity() const { return capacity_; }

    /**
     * @brief Set new rate
     *
     * Updates token refill rate.
     *
     * @param rate_per_second New rate (tokens per second)
     *
     * @code
     * limiter.set_rate(20.0);  // Change to 20/sec
     * @endcode
     *
     * @note Thread-safe
     * @note Takes effect immediately
     */
    void set_rate(double rate_per_second);

    /**
     * @brief Set new capacity
     *
     * Updates bucket capacity. If new capacity is lower than
     * current tokens, tokens are capped at new capacity.
     *
     * @param capacity New capacity
     *
     * @code
     * limiter.set_capacity(50);  // Change to 50 tokens max
     * @endcode
     *
     * @note Thread-safe
     * @note Current tokens capped to new capacity
     */
    void set_capacity(size_t capacity);

private:
    double rate_per_second_;                 ///< Token refill rate
    size_t capacity_;                        ///< Bucket capacity

    std::atomic<double> tokens_;             ///< Current token count
    std::atomic<uint64_t> last_refill_ns_;   ///< Last refill timestamp (nanoseconds)

    /**
     * @brief Refill tokens based on elapsed time
     *
     * Calculates tokens to add based on time elapsed since last refill.
     * Called before each acquire attempt.
     *
     * @note Thread-safe via atomic operations
     * @note Uses compare-and-swap for lock-free operation
     */
    void refill();

    /**
     * @brief Get current time in nanoseconds
     *
     * @return uint64_t Current time since epoch (nanoseconds)
     */
    static uint64_t now_ns();
};

/**
 * @brief Per-exchange rate limiter manager
 *
 * Manages separate rate limiters for each exchange based on their
 * specific rate limits.
 *
 * **Default Limits:**
 * - Binance: 20 req/sec (1200 req/min), burst 40
 * - Coinbase: 30 req/sec (recommended), burst 60
 * - Kraken: 15 req/sec (conservative), burst 30
 *
 * @code
 * ExchangeRateLimiters limiters;
 *
 * // Acquire for specific exchange
 * limiters.acquire(Exchange::BINANCE);
 * send_binance_request();
 *
 * // Try acquire
 * if (limiters.try_acquire(Exchange::COINBASE)) {
 *     send_coinbase_request();
 * }
 * @endcode
 */
class ExchangeRateLimiters {
public:
    /**
     * @brief Construct with default limits
     *
     * Creates rate limiters for all exchanges with default limits.
     */
    ExchangeRateLimiters();

    /**
     * @brief Acquire token for exchange (blocking)
     *
     * @param exchange Exchange to acquire for
     * @param tokens Number of tokens (default: 1)
     *
     * @note Thread-safe
     */
    void acquire(Exchange exchange, size_t tokens = 1);

    /**
     * @brief Try acquire token for exchange (non-blocking)
     *
     * @param exchange Exchange to acquire for
     * @param tokens Number of tokens (default: 1)
     * @return bool True if acquired
     *
     * @note Thread-safe
     */
    bool try_acquire(Exchange exchange, size_t tokens = 1);

    /**
     * @brief Get rate limiter for exchange
     *
     * @param exchange Exchange
     * @return RateLimiter* Pointer to limiter (may be nullptr)
     *
     * @note Thread-safe
     */
    RateLimiter* get_limiter(Exchange exchange);

    /**
     * @brief Set custom rate for exchange
     *
     * @param exchange Exchange
     * @param rate_per_second New rate
     *
     * @note Thread-safe
     */
    void set_rate(Exchange exchange, double rate_per_second);

private:
    std::unique_ptr<RateLimiter> binance_limiter_;
    std::unique_ptr<RateLimiter> coinbase_limiter_;
    std::unique_ptr<RateLimiter> kraken_limiter_;
};

} // namespace marketdata
