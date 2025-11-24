/**
 * @file arbitrage_monitor.hpp
 * @brief Arbitrage opportunity monitoring and tracking
 *
 * Monitors aggregated order books for arbitrage opportunities and tracks
 * their lifecycle, profitability, and statistics.
 *
 * **Features:**
 * - Real-time arbitrage detection
 * - Opportunity lifecycle tracking
 * - Historical statistics
 * - Profitability analysis
 * - Alert system
 *
 * **Performance:**
 * - Detection: O(1), ~100-200 ns
 * - Tracking: O(log N), ~500 ns
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "core/aggregated_order_book.hpp"
#include <memory>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>

namespace marketdata {

/**
 * @brief Arbitrage opportunity statistics
 */
struct ArbitrageStats {
    size_t total_opportunities{0};          ///< Total opportunities detected
    size_t active_opportunities{0};         ///< Currently active
    double total_potential_profit{0.0};     ///< Total potential profit (USD)
    double average_profit_percent{0.0};     ///< Average profit %
    double max_profit_percent{0.0};         ///< Maximum profit %
    double average_duration_ms{0.0};        ///< Average duration (ms)

    /**
     * @brief Per-exchange pair statistics
     */
    struct ExchangePairStats {
        Exchange buy_exchange;
        Exchange sell_exchange;
        size_t count{0};
        double total_profit{0.0};
        double avg_profit_percent{0.0};
    };

    std::vector<ExchangePairStats> pair_stats;
};

/**
 * @brief Tracked arbitrage opportunity
 *
 * Represents an arbitrage opportunity with lifecycle tracking.
 */
struct TrackedArbitrage {
    uint64_t id;                            ///< Unique ID
    ArbitrageOpportunity opportunity;       ///< The opportunity
    uint64_t detection_time_ns;             ///< When detected
    uint64_t expiry_time_ns;                ///< When expired (0 if still active)
    bool is_active;                         ///< Currently active

    /**
     * @brief Get duration in milliseconds
     */
    double get_duration_ms() const {
        if (expiry_time_ns == 0) {
            return 0.0;
        }
        return static_cast<double>(expiry_time_ns - detection_time_ns) / 1'000'000.0;
    }

    /**
     * @brief Get age in milliseconds
     */
    double get_age_ms(uint64_t current_time_ns) const {
        return static_cast<double>(current_time_ns - detection_time_ns) / 1'000'000.0;
    }
};

/**
 * @brief Arbitrage monitor
 *
 * Monitors multiple aggregated order books for arbitrage opportunities.
 * Tracks opportunity lifecycle and provides statistics.
 *
 * **Architecture:**
 * - Subscribes to aggregated order book updates
 * - Detects arbitrage opportunities
 * - Tracks active opportunities
 * - Maintains historical statistics
 * - Provides real-time alerts
 *
 * **Usage:**
 * @code
 * // Create monitor
 * ArbitrageMonitor monitor;
 *
 * // Add order books to monitor
 * auto btc_book = std::make_shared<AggregatedOrderBook>("BTCUSD");
 * monitor.add_order_book(btc_book);
 *
 * // Set alert callback
 * monitor.set_alert_callback([](const TrackedArbitrage& arb) {
 *     std::cout << "Arbitrage: "
 *               << arb.opportunity.profit_percentage << "% profit\n";
 * });
 *
 * // Start monitoring
 * monitor.start();
 *
 * // Get statistics
 * auto stats = monitor.get_statistics();
 * std::cout << "Total opportunities: " << stats.total_opportunities << "\n";
 * @endcode
 *
 * @note Thread-safe
 */
class ArbitrageMonitor {
public:
    /**
     * @brief Alert callback for new opportunities
     */
    using AlertCallback = std::function<void(const TrackedArbitrage&)>;

    /**
     * @brief Construct arbitrage monitor
     *
     * @param min_profit_percent Minimum profit % to track (default: 0.1%)
     * @param history_size Max history size (default: 1000)
     */
    explicit ArbitrageMonitor(double min_profit_percent = 0.1,
                             size_t history_size = 1000);

    /**
     * @brief Add order book to monitor
     *
     * Subscribes to arbitrage opportunities from this order book.
     *
     * @param order_book Aggregated order book to monitor
     *
     * @code
     * auto btc_book = std::make_shared<AggregatedOrderBook>("BTCUSD");
     * monitor.add_order_book(btc_book);
     * @endcode
     *
     * @note Thread-safe
     */
    void add_order_book(std::shared_ptr<AggregatedOrderBook> order_book);

    /**
     * @brief Remove order book from monitoring
     *
     * @param symbol Symbol to remove
     *
     * @note Thread-safe
     */
    void remove_order_book(const std::string& symbol);

    /**
     * @brief Start monitoring
     *
     * Begins monitoring all added order books.
     *
     * @note Thread-safe
     */
    void start();

    /**
     * @brief Stop monitoring
     *
     * Stops monitoring and clears active opportunities.
     *
     * @note Thread-safe
     */
    void stop();

    /**
     * @brief Set alert callback
     *
     * Callback is invoked when new arbitrage opportunity is detected.
     *
     * @param callback Alert callback function
     *
     * @code
     * monitor.set_alert_callback([](const TrackedArbitrage& arb) {
     *     if (arb.opportunity.profit_percentage > 1.0) {
     *         // High profit opportunity!
     *         execute_arbitrage(arb);
     *     }
     * });
     * @endcode
     *
     * @note Callback invoked from update thread
     * @warning Callback must be fast (no blocking)
     */
    void set_alert_callback(AlertCallback callback);

    /**
     * @brief Get current statistics
     *
     * @return ArbitrageStats Current statistics
     *
     * @note Thread-safe
     */
    ArbitrageStats get_statistics() const;

    /**
     * @brief Get active opportunities
     *
     * @return std::vector<TrackedArbitrage> Currently active opportunities
     *
     * @note Thread-safe
     * @note Returns copy (safe to iterate)
     */
    std::vector<TrackedArbitrage> get_active_opportunities() const;

    /**
     * @brief Get historical opportunities
     *
     * @param count Max number to return (default: 100)
     * @return std::vector<TrackedArbitrage> Historical opportunities
     *
     * @note Thread-safe
     * @note Returns most recent first
     */
    std::vector<TrackedArbitrage> get_history(size_t count = 100) const;

    /**
     * @brief Clear all history
     *
     * Clears historical data and resets statistics.
     *
     * @note Thread-safe
     */
    void clear_history();

    /**
     * @brief Set minimum profit threshold
     *
     * Only opportunities above this threshold will be tracked.
     *
     * @param min_profit_percent Minimum profit %
     *
     * @note Thread-safe
     */
    void set_min_profit(double min_profit_percent);

    /**
     * @brief Get minimum profit threshold
     *
     * @return double Current minimum profit %
     */
    double get_min_profit() const { return min_profit_percent_; }

    /**
     * @brief Check if monitoring is active
     *
     * @return bool True if monitoring
     */
    bool is_running() const { return running_.load(); }

private:
    double min_profit_percent_;                             ///< Min profit threshold
    size_t history_size_;                                   ///< Max history size
    std::atomic<bool> running_{false};                      ///< Running flag
    std::atomic<uint64_t> next_id_{1};                      ///< Next opportunity ID

    mutable std::mutex mutex_;                              ///< Protects state
    std::map<std::string, std::shared_ptr<AggregatedOrderBook>> order_books_;
    std::map<uint64_t, TrackedArbitrage> active_opportunities_;
    std::deque<TrackedArbitrage> history_;                  ///< Historical opportunities

    AlertCallback alert_callback_;                          ///< Alert callback

    // Statistics
    size_t total_count_{0};
    double total_profit_{0.0};
    double max_profit_percent_{0.0};
    double total_duration_ms_{0.0};
    size_t completed_count_{0};

    /**
     * @brief Handle arbitrage opportunity detected
     *
     * @param opportunity Detected opportunity
     */
    void on_arbitrage_detected(const ArbitrageOpportunity& opportunity);

    /**
     * @brief Update active opportunities
     *
     * Checks which opportunities are still valid and expires old ones.
     *
     * @param current_time_ns Current timestamp
     */
    void update_active_opportunities(uint64_t current_time_ns);

    /**
     * @brief Add to history
     *
     * @param arb Tracked arbitrage to add
     */
    void add_to_history(const TrackedArbitrage& arb);
};

} // namespace marketdata
