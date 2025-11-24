/**
 * @file rate_limiter.cpp
 * @brief Rate limiter implementation
 */

#include "utils/rate_limiter.hpp"
#include "utils/time_utils.hpp"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace marketdata {

RateLimiter::RateLimiter(double rate_per_second, size_t capacity)
    : rate_per_second_(rate_per_second)
    , capacity_(capacity)
    , tokens_(static_cast<double>(capacity))
    , last_refill_ns_(now_ns()) {

    if (rate_per_second <= 0) {
        spdlog::warn("RateLimiter: Invalid rate {}, using 1.0", rate_per_second);
        rate_per_second_ = 1.0;
    }

    if (capacity == 0) {
        spdlog::warn("RateLimiter: Invalid capacity 0, using 1");
        capacity_ = 1;
    }

    spdlog::debug("RateLimiter: Created with rate {} tokens/sec, capacity {}",
                 rate_per_second_, capacity_);
}

void RateLimiter::acquire(size_t tokens) {
    while (!try_acquire(tokens)) {
        // Calculate wait time
        double current_tokens = tokens_.load(std::memory_order_relaxed);
        double needed = static_cast<double>(tokens) - current_tokens;

        if (needed > 0 && rate_per_second_ > 0) {
            // Wait for needed tokens to be generated
            uint64_t wait_ns = static_cast<uint64_t>(
                (needed / rate_per_second_) * 1'000'000'000.0);

            // Don't wait more than 100ms at a time
            wait_ns = std::min(wait_ns, 100'000'000ULL);

            std::this_thread::sleep_for(std::chrono::nanoseconds(wait_ns));
        } else {
            // Small sleep to avoid busy waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

bool RateLimiter::try_acquire(size_t tokens) {
    // Refill tokens based on elapsed time
    refill();

    // Try to consume tokens
    double current = tokens_.load(std::memory_order_acquire);
    double needed = static_cast<double>(tokens);

    while (current >= needed) {
        double new_value = current - needed;

        // Try to atomically update token count
        if (tokens_.compare_exchange_weak(current, new_value,
                                         std::memory_order_release,
                                         std::memory_order_acquire)) {
            return true;
        }
        // CAS failed, current was updated with actual value, retry
    }

    return false;
}

bool RateLimiter::try_acquire_for(uint64_t timeout_ms, size_t tokens) {
    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() - start < timeout) {
        if (try_acquire(tokens)) {
            return true;
        }

        // Calculate remaining time and wait
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto remaining = timeout - elapsed;

        if (remaining <= std::chrono::milliseconds(0)) {
            break;
        }

        // Wait for up to 10ms or remaining time, whichever is smaller
        auto wait_time = std::min(remaining, std::chrono::milliseconds(10));
        std::this_thread::sleep_for(wait_time);
    }

    return false;
}

void RateLimiter::reset() {
    tokens_.store(static_cast<double>(capacity_), std::memory_order_release);
    last_refill_ns_.store(now_ns(), std::memory_order_release);
    spdlog::debug("RateLimiter: Reset to capacity {}", capacity_);
}

double RateLimiter::get_current_tokens() const {
    // Note: This is racy, but acceptable for monitoring
    const_cast<RateLimiter*>(this)->refill();
    return tokens_.load(std::memory_order_relaxed);
}

void RateLimiter::set_rate(double rate_per_second) {
    if (rate_per_second <= 0) {
        spdlog::warn("RateLimiter: Invalid rate {}, ignoring", rate_per_second);
        return;
    }

    rate_per_second_ = rate_per_second;
    spdlog::debug("RateLimiter: Rate set to {} tokens/sec", rate_per_second_);
}

void RateLimiter::set_capacity(size_t capacity) {
    if (capacity == 0) {
        spdlog::warn("RateLimiter: Invalid capacity 0, ignoring");
        return;
    }

    capacity_ = capacity;

    // Cap current tokens to new capacity
    double current = tokens_.load(std::memory_order_acquire);
    double cap = static_cast<double>(capacity);

    while (current > cap) {
        if (tokens_.compare_exchange_weak(current, cap,
                                         std::memory_order_release,
                                         std::memory_order_acquire)) {
            break;
        }
    }

    spdlog::debug("RateLimiter: Capacity set to {}", capacity_);
}

void RateLimiter::refill() {
    uint64_t now = now_ns();
    uint64_t last = last_refill_ns_.load(std::memory_order_acquire);

    // Calculate elapsed time in seconds
    double elapsed_sec = static_cast<double>(now - last) / 1'000'000'000.0;

    // Calculate tokens to add
    double tokens_to_add = elapsed_sec * rate_per_second_;

    if (tokens_to_add < 0.001) {
        // Not enough time elapsed
        return;
    }

    // Try to update last_refill_ns atomically
    if (!last_refill_ns_.compare_exchange_strong(last, now,
                                                 std::memory_order_release,
                                                 std::memory_order_acquire)) {
        // Another thread updated it, skip this refill
        return;
    }

    // Add tokens up to capacity
    double current = tokens_.load(std::memory_order_acquire);
    double cap = static_cast<double>(capacity_);

    while (current < cap) {
        double new_value = std::min(current + tokens_to_add, cap);

        if (tokens_.compare_exchange_weak(current, new_value,
                                         std::memory_order_release,
                                         std::memory_order_acquire)) {
            break;
        }
        // CAS failed, current was updated, retry
    }
}

uint64_t RateLimiter::now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ExchangeRateLimiters implementation

ExchangeRateLimiters::ExchangeRateLimiters() {
    // Binance: 1200 req/min = 20 req/sec
    binance_limiter_ = std::make_unique<RateLimiter>(20.0, 40);

    // Coinbase: No explicit limit, use conservative 30 req/sec
    coinbase_limiter_ = std::make_unique<RateLimiter>(30.0, 60);

    // Kraken: Conservative 15 req/sec (public API has high limits)
    kraken_limiter_ = std::make_unique<RateLimiter>(15.0, 30);

    spdlog::info("ExchangeRateLimiters: Initialized with default limits");
}

void ExchangeRateLimiters::acquire(Exchange exchange, size_t tokens) {
    auto* limiter = get_limiter(exchange);
    if (limiter) {
        limiter->acquire(tokens);
    }
}

bool ExchangeRateLimiters::try_acquire(Exchange exchange, size_t tokens) {
    auto* limiter = get_limiter(exchange);
    if (limiter) {
        return limiter->try_acquire(tokens);
    }
    return true;  // No limiter, allow by default
}

RateLimiter* ExchangeRateLimiters::get_limiter(Exchange exchange) {
    switch (exchange) {
        case Exchange::BINANCE:
            return binance_limiter_.get();

        case Exchange::COINBASE:
            return coinbase_limiter_.get();

        case Exchange::KRAKEN:
            return kraken_limiter_.get();

        default:
            spdlog::warn("ExchangeRateLimiters: Unknown exchange {}",
                        static_cast<int>(exchange));
            return nullptr;
    }
}

void ExchangeRateLimiters::set_rate(Exchange exchange, double rate_per_second) {
    auto* limiter = get_limiter(exchange);
    if (limiter) {
        limiter->set_rate(rate_per_second);
        spdlog::info("ExchangeRateLimiters: Set rate for exchange {} to {} req/sec",
                    static_cast<int>(exchange), rate_per_second);
    }
}

} // namespace marketdata
