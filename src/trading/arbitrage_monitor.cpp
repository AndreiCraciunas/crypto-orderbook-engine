/**
 * @file arbitrage_monitor.cpp
 * @brief Arbitrage monitor implementation
 */

#include "trading/arbitrage_monitor.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace marketdata {

ArbitrageMonitor::ArbitrageMonitor(double min_profit_percent, size_t history_size)
    : min_profit_percent_(min_profit_percent)
    , history_size_(history_size) {
    spdlog::info("ArbitrageMonitor: Created (min profit: {}%, history: {})",
                min_profit_percent_, history_size_);
}

void ArbitrageMonitor::add_order_book(std::shared_ptr<AggregatedOrderBook> order_book) {
    if (!order_book) {
        spdlog::warn("ArbitrageMonitor: Null order book");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto& symbol = order_book->get_symbol();

    if (order_books_.find(symbol) != order_books_.end()) {
        spdlog::warn("ArbitrageMonitor: Order book {} already added", symbol);
        return;
    }

    order_books_[symbol] = order_book;

    // Set arbitrage callback
    order_book->set_arbitrage_callback([this](const ArbitrageOpportunity& opp) {
        on_arbitrage_detected(opp);
    });

    spdlog::info("ArbitrageMonitor: Added order book {}", symbol);
}

void ArbitrageMonitor::remove_order_book(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = order_books_.find(symbol);
    if (it == order_books_.end()) {
        spdlog::warn("ArbitrageMonitor: Order book {} not found", symbol);
        return;
    }

    // Clear callback
    it->second->set_arbitrage_callback(nullptr);

    order_books_.erase(it);
    spdlog::info("ArbitrageMonitor: Removed order book {}", symbol);
}

void ArbitrageMonitor::start() {
    if (running_.exchange(true)) {
        spdlog::warn("ArbitrageMonitor: Already running");
        return;
    }

    spdlog::info("ArbitrageMonitor: Started monitoring {} order books",
                order_books_.size());
}

void ArbitrageMonitor::stop() {
    if (!running_.exchange(false)) {
        spdlog::warn("ArbitrageMonitor: Not running");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Expire all active opportunities
    uint64_t now = get_timestamp_ns();
    for (auto& [id, arb] : active_opportunities_) {
        arb.expiry_time_ns = now;
        arb.is_active = false;
        add_to_history(arb);
    }

    active_opportunities_.clear();

    spdlog::info("ArbitrageMonitor: Stopped");
}

void ArbitrageMonitor::set_alert_callback(AlertCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    alert_callback_ = callback;
}

ArbitrageStats ArbitrageMonitor::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);

    ArbitrageStats stats;
    stats.total_opportunities = total_count_;
    stats.active_opportunities = active_opportunities_.size();
    stats.total_potential_profit = total_profit_;
    stats.max_profit_percent = max_profit_percent_;

    if (total_count_ > 0) {
        stats.average_profit_percent = total_profit_ / total_count_;
    }

    if (completed_count_ > 0) {
        stats.average_duration_ms = total_duration_ms_ / completed_count_;
    }

    // Calculate per-exchange-pair statistics
    std::map<std::pair<Exchange, Exchange>, ArbitrageStats::ExchangePairStats> pair_map;

    for (const auto& arb : history_) {
        auto key = std::make_pair(arb.opportunity.buy_exchange,
                                 arb.opportunity.sell_exchange);

        auto& pair_stat = pair_map[key];
        pair_stat.buy_exchange = arb.opportunity.buy_exchange;
        pair_stat.sell_exchange = arb.opportunity.sell_exchange;
        pair_stat.count++;
        pair_stat.total_profit += arb.opportunity.profit_percentage;
    }

    for (auto& [key, pair_stat] : pair_map) {
        if (pair_stat.count > 0) {
            pair_stat.avg_profit_percent = pair_stat.total_profit / pair_stat.count;
        }
        stats.pair_stats.push_back(pair_stat);
    }

    return stats;
}

std::vector<TrackedArbitrage> ArbitrageMonitor::get_active_opportunities() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TrackedArbitrage> active;
    active.reserve(active_opportunities_.size());

    for (const auto& [id, arb] : active_opportunities_) {
        active.push_back(arb);
    }

    // Sort by profit percentage (descending)
    std::sort(active.begin(), active.end(),
        [](const auto& a, const auto& b) {
            return a.opportunity.profit_percentage > b.opportunity.profit_percentage;
        });

    return active;
}

std::vector<TrackedArbitrage> ArbitrageMonitor::get_history(size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TrackedArbitrage> result;
    size_t n = std::min(count, history_.size());
    result.reserve(n);

    // History is stored with most recent first
    for (size_t i = 0; i < n; i++) {
        result.push_back(history_[i]);
    }

    return result;
}

void ArbitrageMonitor::clear_history() {
    std::lock_guard<std::mutex> lock(mutex_);

    history_.clear();
    total_count_ = 0;
    total_profit_ = 0.0;
    max_profit_percent_ = 0.0;
    total_duration_ms_ = 0.0;
    completed_count_ = 0;

    spdlog::info("ArbitrageMonitor: History cleared");
}

void ArbitrageMonitor::set_min_profit(double min_profit_percent) {
    min_profit_percent_ = min_profit_percent;
    spdlog::info("ArbitrageMonitor: Min profit set to {}%", min_profit_percent_);
}

void ArbitrageMonitor::on_arbitrage_detected(const ArbitrageOpportunity& opportunity) {
    if (!running_.load()) {
        return;
    }

    if (opportunity.profit_percentage < min_profit_percent_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Create tracked arbitrage
    TrackedArbitrage tracked;
    tracked.id = next_id_.fetch_add(1);
    tracked.opportunity = opportunity;
    tracked.detection_time_ns = get_timestamp_ns();
    tracked.expiry_time_ns = 0;
    tracked.is_active = true;

    // Add to active opportunities
    active_opportunities_[tracked.id] = tracked;

    // Update statistics
    total_count_++;
    total_profit_ += opportunity.profit_percentage;
    max_profit_percent_ = std::max(max_profit_percent_, opportunity.profit_percentage);

    spdlog::info("ArbitrageMonitor: Opportunity {} detected - Buy {} @ {} on {}, "
                "Sell {} @ {} on {}, Profit: {:.2f}% ({} available)",
                tracked.id,
                opportunity.symbol, opportunity.buy_price,
                static_cast<int>(opportunity.buy_exchange),
                opportunity.symbol, opportunity.sell_price,
                static_cast<int>(opportunity.sell_exchange),
                opportunity.profit_percentage,
                opportunity.max_quantity);

    // Invoke alert callback (without lock)
    if (alert_callback_) {
        // Make a copy for the callback
        TrackedArbitrage arb_copy = tracked;

        // Release lock before callback to avoid deadlock
        mutex_.unlock();
        alert_callback_(arb_copy);
        mutex_.lock();
    }

    // Update active opportunities (expire old ones)
    update_active_opportunities(tracked.detection_time_ns);
}

void ArbitrageMonitor::update_active_opportunities(uint64_t current_time_ns) {
    // Timeout for opportunities (1 second)
    const uint64_t timeout_ns = 1'000'000'000;

    std::vector<uint64_t> to_expire;

    for (auto& [id, arb] : active_opportunities_) {
        uint64_t age_ns = current_time_ns - arb.detection_time_ns;

        if (age_ns > timeout_ns) {
            to_expire.push_back(id);
        }
    }

    // Expire old opportunities
    for (uint64_t id : to_expire) {
        auto it = active_opportunities_.find(id);
        if (it != active_opportunities_.end()) {
            it->second.expiry_time_ns = current_time_ns;
            it->second.is_active = false;

            // Add to history
            add_to_history(it->second);

            // Update duration stats
            double duration_ms = it->second.get_duration_ms();
            total_duration_ms_ += duration_ms;
            completed_count_++;

            spdlog::debug("ArbitrageMonitor: Opportunity {} expired after {:.2f}ms",
                         id, duration_ms);

            active_opportunities_.erase(it);
        }
    }
}

void ArbitrageMonitor::add_to_history(const TrackedArbitrage& arb) {
    // Add to front (most recent first)
    history_.push_front(arb);

    // Trim if exceeds size
    while (history_.size() > history_size_) {
        history_.pop_back();
    }
}

} // namespace marketdata
