/**
 * @file metrics_collector.hpp
 * @brief Performance metrics collection and latency tracking
 *
 * Provides lock-free metrics collection for monitoring order book performance,
 * latency distributions, and throughput. Uses logarithmic histograms for
 * efficient percentile calculation.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <atomic>
#include <array>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace marketdata {

/**
 * @brief Lock-free histogram for latency tracking
 *
 * Implements a thread-safe histogram using atomic operations and logarithmic
 * bucketing for efficient latency distribution tracking. Designed for
 * minimal overhead in hot paths.
 *
 * **Algorithm:**
 * - Logarithmic bucketing: bucket = log2(value) * 10
 * - Atomic counters for each bucket
 * - CAS loops for min/max tracking
 * - Linear scan for percentile calculation
 *
 * **Bucketing Strategy:**
 * - Bucket 0: values 0-1
 * - Bucket 10: values 2-3
 * - Bucket 20: values 4-7
 * - Bucket 30: values 8-15
 * - Higher buckets cover exponentially larger ranges
 *
 * **Performance:**
 * - Record: O(1), ~50-100 ns (atomic increments + CAS for min/max)
 * - Percentile: O(NUM_BUCKETS), ~1-2 μs
 * - Lock-free for concurrent recording
 *
 * @tparam NUM_BUCKETS Number of histogram buckets (default: 1000)
 *
 * @code
 * LockFreeHistogram<1000> latency_hist;
 *
 * // Record latencies from multiple threads
 * latency_hist.record(150);   // 150 ns
 * latency_hist.record(2000);  // 2 μs
 * latency_hist.record(500);   // 500 ns
 *
 * // Get percentiles
 * std::cout << "p50: " << latency_hist.get_percentile(50) << " ns\n";
 * std::cout << "p95: " << latency_hist.get_percentile(95) << " ns\n";
 * std::cout << "p99: " << latency_hist.get_percentile(99) << " ns\n";
 *
 * // Get statistics
 * std::cout << "Min: " << latency_hist.get_min() << " ns\n";
 * std::cout << "Max: " << latency_hist.get_max() << " ns\n";
 * std::cout << "Avg: " << latency_hist.get_average() << " ns\n";
 * std::cout << "Count: " << latency_hist.get_count() << '\n';
 * @endcode
 *
 * @note Thread-safe for concurrent record() calls
 * @note Percentile values are approximate due to logarithmic bucketing
 * @warning reset() is not thread-safe - call when no recording
 * @see MetricsCollector for complete metrics collection
 */
template<size_t NUM_BUCKETS = 1000>
class LockFreeHistogram {
public:
    /**
     * @brief Construct empty histogram
     *
     * Initializes all buckets and statistics to zero.
     */
    LockFreeHistogram() {
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            buckets_[i].store(0, std::memory_order_relaxed);
        }
        min_value_.store(UINT64_MAX, std::memory_order_relaxed);
        max_value_.store(0, std::memory_order_relaxed);
        total_count_.store(0, std::memory_order_relaxed);
        total_sum_.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Record a value
     *
     * Adds value to histogram, updating bucket counter and min/max/count/sum.
     * Uses logarithmic bucketing for wide range coverage.
     *
     * @param value Value to record (typically latency in nanoseconds)
     *
     * @code
     * LockFreeHistogram<1000> hist;
     *
     * // Record latency measurement
     * uint64_t start = get_timestamp_ns();
     * process_update();
     * uint64_t latency = get_timestamp_ns() - start;
     * hist.record(latency);
     * @endcode
     *
     * @note Thread-safe for concurrent calls
     * @note Uses CAS loops for min/max - may retry under contention
     */
    void record(uint64_t value) {
        size_t bucket = value_to_bucket(value);
        if (bucket < NUM_BUCKETS) {
            buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        }

        // Update min/max
        uint64_t current_min = min_value_.load(std::memory_order_relaxed);
        while (value < current_min &&
               !min_value_.compare_exchange_weak(current_min, value,
                                                  std::memory_order_relaxed)) {}

        uint64_t current_max = max_value_.load(std::memory_order_relaxed);
        while (value > current_max &&
               !max_value_.compare_exchange_weak(current_max, value,
                                                  std::memory_order_relaxed)) {}

        total_count_.fetch_add(1, std::memory_order_relaxed);
        total_sum_.fetch_add(value, std::memory_order_relaxed);
    }

    /**
     * @brief Get percentile value
     *
     * Calculates approximate percentile by scanning buckets until
     * cumulative count reaches target. Result is approximate due to
     * logarithmic bucketing.
     *
     * @param percentile Percentile to calculate (0-100)
     * @return uint64_t Approximate value at percentile
     *
     * @code
     * LockFreeHistogram<1000> hist;
     * // ... record values ...
     *
     * std::cout << "Median: " << hist.get_percentile(50) << " ns\n";
     * std::cout << "p95: " << hist.get_percentile(95) << " ns\n";
     * std::cout << "p99: " << hist.get_percentile(99) << " ns\n";
     * std::cout << "p99.9: " << hist.get_percentile(99.9) << " ns\n";
     * @endcode
     *
     * @note O(NUM_BUCKETS) complexity
     * @note Returns 0 if no values recorded
     * @note Result is approximate due to bucket granularity
     */
    uint64_t get_percentile(double percentile) const {
        uint64_t total = total_count_.load(std::memory_order_acquire);
        if (total == 0) {
            return 0;
        }

        uint64_t target_count = static_cast<uint64_t>(total * percentile / 100.0);
        uint64_t cumulative = 0;

        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            cumulative += buckets_[i].load(std::memory_order_acquire);
            if (cumulative >= target_count) {
                return bucket_to_value(i);
            }
        }

        return max_value_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get average value
     *
     * Calculates arithmetic mean of all recorded values.
     *
     * @return double Average value, or 0.0 if no values
     *
     * @code
     * double avg_latency = hist.get_average();
     * std::cout << "Average latency: " << avg_latency << " ns\n";
     * @endcode
     */
    double get_average() const {
        uint64_t total = total_count_.load(std::memory_order_acquire);
        if (total == 0) {
            return 0.0;
        }

        uint64_t sum = total_sum_.load(std::memory_order_acquire);
        return static_cast<double>(sum) / static_cast<double>(total);
    }

    /**
     * @brief Get minimum value
     *
     * Returns smallest value recorded.
     *
     * @return uint64_t Minimum value, or 0 if no values
     */
    uint64_t get_min() const {
        uint64_t min = min_value_.load(std::memory_order_acquire);
        return min == UINT64_MAX ? 0 : min;
    }

    /**
     * @brief Get maximum value
     *
     * Returns largest value recorded.
     *
     * @return uint64_t Maximum value, or 0 if no values
     */
    uint64_t get_max() const {
        return max_value_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get total count
     *
     * Returns number of values recorded.
     *
     * @return uint64_t Total number of recorded values
     */
    uint64_t get_count() const {
        return total_count_.load(std::memory_order_acquire);
    }

    /**
     * @brief Reset histogram
     *
     * Clears all buckets and resets statistics to zero.
     *
     * @code
     * hist.reset();  // Start fresh
     * @endcode
     *
     * @warning Not thread-safe - ensure no concurrent record() calls
     */
    void reset() {
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            buckets_[i].store(0, std::memory_order_relaxed);
        }
        min_value_.store(UINT64_MAX, std::memory_order_relaxed);
        max_value_.store(0, std::memory_order_relaxed);
        total_count_.store(0, std::memory_order_relaxed);
        total_sum_.store(0, std::memory_order_relaxed);
    }

private:
    /**
     * @brief Map value to bucket using logarithmic bucketing
     *
     * Uses log2(value) * 10 for exponential bucket spacing.
     *
     * @param value Value to bucket
     * @return size_t Bucket index
     */
    size_t value_to_bucket(uint64_t value) const {
        if (value == 0) return 0;

        // Logarithmic bucketing
        double log_val = std::log2(static_cast<double>(value));
        size_t bucket = static_cast<size_t>(log_val * 10);

        return std::min(bucket, NUM_BUCKETS - 1);
    }

    /**
     * @brief Map bucket to approximate value
     *
     * Inverse of value_to_bucket. Returns midpoint value for bucket.
     *
     * @param bucket Bucket index
     * @return uint64_t Approximate value
     */
    uint64_t bucket_to_value(size_t bucket) const {
        double log_val = static_cast<double>(bucket) / 10.0;
        return static_cast<uint64_t>(std::pow(2.0, log_val));
    }

    alignas(64) std::array<std::atomic<uint64_t>, NUM_BUCKETS> buckets_;  ///< Histogram buckets
    alignas(64) std::atomic<uint64_t> min_value_;   ///< Minimum value seen
    alignas(64) std::atomic<uint64_t> max_value_;   ///< Maximum value seen
    alignas(64) std::atomic<uint64_t> total_count_; ///< Total sample count
    alignas(64) std::atomic<uint64_t> total_sum_;   ///< Sum for average calculation
};

/**
 * @brief Metrics collector for order book operations
 *
 * Comprehensive metrics collection for monitoring order book performance,
 * latency distributions, throughput, and market data statistics. All
 * operations are lock-free and designed for minimal hot-path overhead.
 *
 * **Tracked Metrics:**
 * - Update latency distribution (p50, p95, p99, p99.9)
 * - Snapshot generation latency distribution
 * - Updates per second (throughput)
 * - Snapshots generated
 * - Total bid/ask volumes
 * - Spread, mid price, book imbalance
 *
 * **Usage Pattern:**
 * 1. Record latencies and increment counters during operations
 * 2. Periodically read metrics for monitoring/logging
 * 3. Call reset_counters() periodically (e.g., every second) for rate metrics
 *
 * @code
 * MetricsCollector metrics;
 *
 * // Record update latency
 * uint64_t start = get_timestamp_ns();
 * order_book.update_bid(price, qty);
 * uint64_t latency = get_timestamp_ns() - start;
 * metrics.record_update_latency_ns(latency);
 * metrics.increment_updates();
 *
 * // Record snapshot latency
 * start = get_timestamp_ns();
 * auto snapshot = order_book.get_snapshot();
 * latency = get_timestamp_ns() - start;
 * metrics.record_snapshot_latency_ns(latency);
 * metrics.increment_snapshots();
 *
 * // Update market statistics
 * metrics.set_spread(order_book.get_spread());
 * metrics.set_mid_price(order_book.get_mid_price());
 *
 * // Get summary for monitoring
 * auto summary = metrics.get_summary();
 * std::cout << "Updates/sec: " << summary.messages_per_second << '\n';
 * std::cout << "p99 latency: " << summary.latency_p99_us << " μs\n";
 * std::cout << "Spread: " << summary.spread << '\n';
 *
 * // Reset counters every second
 * metrics.reset_counters();
 * @endcode
 *
 * @note All methods are thread-safe
 * @note Minimal overhead for hot-path operations
 * @see LockFreeHistogram for latency distribution tracking
 */
class MetricsCollector {
public:
    /**
     * @brief Construct metrics collector
     *
     * Initializes all metrics to zero.
     */
    MetricsCollector() {
        reset();
    }

    /**
     * @brief Record update latency
     *
     * Adds order book update latency to histogram.
     *
     * @param latency_ns Latency in nanoseconds
     *
     * @code
     * uint64_t start = get_timestamp_ns();
     * book.update_bid(price, qty);
     * metrics.record_update_latency_ns(get_timestamp_ns() - start);
     * @endcode
     *
     * @note Thread-safe
     * @see increment_updates()
     */
    void record_update_latency_ns(uint64_t latency_ns) {
        update_latency_ns_.record(latency_ns);
    }

    /**
     * @brief Record snapshot generation latency
     *
     * Adds snapshot generation latency to histogram.
     *
     * @param latency_ns Latency in nanoseconds
     *
     * @code
     * uint64_t start = get_timestamp_ns();
     * auto snapshot = book.get_snapshot(20);
     * metrics.record_snapshot_latency_ns(get_timestamp_ns() - start);
     * @endcode
     *
     * @note Thread-safe
     * @see increment_snapshots()
     */
    void record_snapshot_latency_ns(uint64_t latency_ns) {
        snapshot_latency_ns_.record(latency_ns);
    }

    /**
     * @brief Increment update counter
     *
     * Increments count of updates processed. Used for throughput calculation.
     *
     * @code
     * book.update_bid(price, qty);
     * metrics.increment_updates();
     * @endcode
     *
     * @note Thread-safe
     * @see get_updates_per_second()
     * @see reset_counters()
     */
    void increment_updates() {
        updates_per_second_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Increment snapshot counter
     *
     * Increments count of snapshots generated.
     *
     * @code
     * auto snapshot = book.get_snapshot();
     * metrics.increment_snapshots();
     * @endcode
     *
     * @note Thread-safe
     * @see get_snapshots_generated()
     */
    void increment_snapshots() {
        snapshots_generated_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Set total bid volume
     *
     * Updates total bid volume metric.
     *
     * @param volume Total bid volume (in fixed-point or double)
     *
     * @note Thread-safe
     */
    void set_total_bid_volume(uint64_t volume) {
        total_bid_volume_.store(volume, std::memory_order_release);
    }

    /**
     * @brief Set total ask volume
     *
     * Updates total ask volume metric.
     *
     * @param volume Total ask volume (in fixed-point or double)
     *
     * @note Thread-safe
     */
    void set_total_ask_volume(uint64_t volume) {
        total_ask_volume_.store(volume, std::memory_order_release);
    }

    /**
     * @brief Set spread
     *
     * Updates bid-ask spread metric. Stores double as bit pattern
     * in atomic uint64_t for lock-free operation.
     *
     * @param spread Bid-ask spread
     *
     * @code
     * metrics.set_spread(book.get_spread());
     * @endcode
     *
     * @note Thread-safe
     */
    void set_spread(double spread) {
        uint64_t spread_bits;
        std::memcpy(&spread_bits, &spread, sizeof(double));
        spread_.store(spread_bits, std::memory_order_release);
    }

    /**
     * @brief Set mid price
     *
     * Updates mid price metric. Stores double as bit pattern.
     *
     * @param mid_price Mid price
     *
     * @code
     * metrics.set_mid_price(book.get_mid_price());
     * @endcode
     *
     * @note Thread-safe
     */
    void set_mid_price(double mid_price) {
        uint64_t mid_price_bits;
        std::memcpy(&mid_price_bits, &mid_price, sizeof(double));
        mid_price_.store(mid_price_bits, std::memory_order_release);
    }

    /**
     * @brief Set book imbalance
     *
     * Updates order book imbalance metric. Stores double as bit pattern.
     *
     * @param imbalance Book imbalance (-1.0 to 1.0)
     *
     * @code
     * metrics.set_book_imbalance(book.get_book_imbalance());
     * @endcode
     *
     * @note Thread-safe
     */
    void set_book_imbalance(double imbalance) {
        uint64_t imbalance_bits;
        std::memcpy(&imbalance_bits, &imbalance, sizeof(double));
        book_imbalance_.store(imbalance_bits, std::memory_order_release);
    }

    /**
     * @brief Get update latency p50 (median)
     * @return uint64_t Median latency in nanoseconds
     */
    uint64_t get_update_latency_p50() const { return update_latency_ns_.get_percentile(50); }

    /**
     * @brief Get update latency p95
     * @return uint64_t 95th percentile latency in nanoseconds
     */
    uint64_t get_update_latency_p95() const { return update_latency_ns_.get_percentile(95); }

    /**
     * @brief Get update latency p99
     * @return uint64_t 99th percentile latency in nanoseconds
     */
    uint64_t get_update_latency_p99() const { return update_latency_ns_.get_percentile(99); }

    /**
     * @brief Get update latency p99.9
     * @return uint64_t 99.9th percentile latency in nanoseconds
     */
    uint64_t get_update_latency_p999() const { return update_latency_ns_.get_percentile(99.9); }

    /**
     * @brief Get snapshot latency p50 (median)
     * @return uint64_t Median snapshot latency in nanoseconds
     */
    uint64_t get_snapshot_latency_p50() const { return snapshot_latency_ns_.get_percentile(50); }

    /**
     * @brief Get snapshot latency p95
     * @return uint64_t 95th percentile snapshot latency in nanoseconds
     */
    uint64_t get_snapshot_latency_p95() const { return snapshot_latency_ns_.get_percentile(95); }

    /**
     * @brief Get snapshot latency p99
     * @return uint64_t 99th percentile snapshot latency in nanoseconds
     */
    uint64_t get_snapshot_latency_p99() const { return snapshot_latency_ns_.get_percentile(99); }

    /**
     * @brief Get updates per second
     *
     * Returns count of updates since last reset_counters().
     *
     * @return uint64_t Update count
     *
     * @note Call reset_counters() periodically for rate calculation
     */
    uint64_t get_updates_per_second() const {
        return updates_per_second_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get snapshots generated
     *
     * Returns count of snapshots since last reset_counters().
     *
     * @return uint64_t Snapshot count
     */
    uint64_t get_snapshots_generated() const {
        return snapshots_generated_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get total bid volume
     * @return uint64_t Total bid volume
     */
    uint64_t get_total_bid_volume() const {
        return total_bid_volume_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get total ask volume
     * @return uint64_t Total ask volume
     */
    uint64_t get_total_ask_volume() const {
        return total_ask_volume_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get current spread
     * @return double Bid-ask spread
     */
    double get_spread() const {
        uint64_t spread_bits = spread_.load(std::memory_order_acquire);
        double spread;
        std::memcpy(&spread, &spread_bits, sizeof(double));
        return spread;
    }

    /**
     * @brief Get current mid price
     * @return double Mid price
     */
    double get_mid_price() const {
        uint64_t mid_price_bits = mid_price_.load(std::memory_order_acquire);
        double mid_price;
        std::memcpy(&mid_price, &mid_price_bits, sizeof(double));
        return mid_price;
    }

    /**
     * @brief Get current book imbalance
     * @return double Book imbalance (-1.0 to 1.0)
     */
    double get_book_imbalance() const {
        uint64_t imbalance_bits = book_imbalance_.load(std::memory_order_acquire);
        double imbalance;
        std::memcpy(&imbalance, &imbalance_bits, sizeof(double));
        return imbalance;
    }

    /**
     * @brief Reset throughput counters
     *
     * Resets update and snapshot counters to zero. Call this
     * periodically (e.g., every second) to calculate rates.
     *
     * @code
     * // In monitoring thread
     * while (running) {
     *     auto updates = metrics.get_updates_per_second();
     *     std::cout << "Updates/sec: " << updates << '\n';
     *     metrics.reset_counters();
     *     std::this_thread::sleep_for(std::chrono::seconds(1));
     * }
     * @endcode
     *
     * @note Thread-safe
     * @see reset() to reset all metrics
     */
    void reset_counters() {
        updates_per_second_.store(0, std::memory_order_release);
        snapshots_generated_.store(0, std::memory_order_release);
    }

    /**
     * @brief Reset all metrics
     *
     * Resets all histograms, counters, and statistics to zero.
     *
     * @warning Not thread-safe for histograms - ensure no concurrent recording
     */
    void reset() {
        update_latency_ns_.reset();
        snapshot_latency_ns_.reset();
        updates_per_second_.store(0, std::memory_order_release);
        snapshots_generated_.store(0, std::memory_order_release);
        total_bid_volume_.store(0, std::memory_order_release);
        total_ask_volume_.store(0, std::memory_order_release);
        spread_.store(0, std::memory_order_release);
        mid_price_.store(0, std::memory_order_release);
        book_imbalance_.store(0, std::memory_order_release);
    }

    /**
     * @brief Metrics summary structure
     *
     * Aggregated metrics for monitoring dashboards.
     */
    struct Summary {
        uint64_t messages_per_second;  ///< Updates processed per second
        uint64_t active_connections;   ///< Active WebSocket connections
        uint64_t latency_p50_us;       ///< Median latency (microseconds)
        uint64_t latency_p95_us;       ///< p95 latency (microseconds)
        uint64_t latency_p99_us;       ///< p99 latency (microseconds)
        double spread;                 ///< Current spread
        double mid_price;              ///< Current mid price
        double book_imbalance;         ///< Current imbalance
    };

    /**
     * @brief Get metrics summary
     *
     * Returns aggregated metrics suitable for logging/monitoring.
     *
     * @return Summary Metrics summary
     *
     * @code
     * auto summary = metrics.get_summary();
     * logger.info("Updates/sec: {}, p99: {}μs, Spread: {}",
     *             summary.messages_per_second,
     *             summary.latency_p99_us,
     *             summary.spread);
     * @endcode
     */
    Summary get_summary() const {
        Summary summary;
        summary.messages_per_second = get_updates_per_second();
        summary.active_connections = 0; // Will be set by WebSocket server
        summary.latency_p50_us = get_update_latency_p50() / 1000;
        summary.latency_p95_us = get_update_latency_p95() / 1000;
        summary.latency_p99_us = get_update_latency_p99() / 1000;
        summary.spread = get_spread();
        summary.mid_price = get_mid_price();
        summary.book_imbalance = get_book_imbalance();
        return summary;
    }

private:
    // Latency tracking
    LockFreeHistogram<1000> update_latency_ns_;    ///< Update latency histogram
    LockFreeHistogram<1000> snapshot_latency_ns_;  ///< Snapshot latency histogram

    // Throughput counters
    alignas(64) std::atomic<uint64_t> updates_per_second_{0};   ///< Update counter
    alignas(64) std::atomic<uint64_t> snapshots_generated_{0};  ///< Snapshot counter

    // Book statistics
    alignas(64) std::atomic<uint64_t> total_bid_volume_{0};  ///< Total bid volume
    alignas(64) std::atomic<uint64_t> total_ask_volume_{0};  ///< Total ask volume
    alignas(64) std::atomic<uint64_t> spread_{0};            ///< Spread (stored as bits)
    alignas(64) std::atomic<uint64_t> mid_price_{0};         ///< Mid price (stored as bits)
    alignas(64) std::atomic<uint64_t> book_imbalance_{0};    ///< Imbalance (stored as bits)
};

} // namespace marketdata
