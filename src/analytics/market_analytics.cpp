/**
 * @file market_analytics.cpp
 * @brief Market analytics implementation
 */

#include "analytics/market_analytics.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <cmath>
#include <numeric>

namespace marketdata {

// VWAPCalculator implementation

VWAPCalculator::VWAPCalculator(uint64_t window_ns)
    : window_ns_(window_ns) {
}

void VWAPCalculator::update(double price, double volume, uint64_t timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove expired trades
    remove_expired(timestamp_ns);

    // Add new trade
    TradePoint trade;
    trade.price = price;
    trade.volume = volume;
    trade.timestamp_ns = timestamp_ns;

    trades_.push_back(trade);

    // Update cumulative values
    cumulative_price_volume_ += price * volume;
    cumulative_volume_ += volume;
}

double VWAPCalculator::get_vwap() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cumulative_volume_ == 0.0) {
        return 0.0;
    }

    return cumulative_price_volume_ / cumulative_volume_;
}

double VWAPCalculator::get_total_volume() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cumulative_volume_;
}

void VWAPCalculator::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    trades_.clear();
    cumulative_price_volume_ = 0.0;
    cumulative_volume_ = 0.0;
}

void VWAPCalculator::remove_expired(uint64_t current_time_ns) {
    uint64_t cutoff = current_time_ns - window_ns_;

    while (!trades_.empty() && trades_.front().timestamp_ns < cutoff) {
        const auto& trade = trades_.front();
        cumulative_price_volume_ -= trade.price * trade.volume;
        cumulative_volume_ -= trade.volume;
        trades_.pop_front();
    }
}

// TWAPCalculator implementation

TWAPCalculator::TWAPCalculator(uint64_t window_ns)
    : window_ns_(window_ns) {
}

void TWAPCalculator::update(double price, uint64_t timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove expired samples
    remove_expired(timestamp_ns);

    // Update duration of last sample
    if (!samples_.empty() && last_timestamp_ns_ > 0) {
        samples_.back().duration_ns = timestamp_ns - last_timestamp_ns_;
    }

    // Add new sample
    PriceSample sample;
    sample.price = price;
    sample.timestamp_ns = timestamp_ns;
    sample.duration_ns = 0;  // Will be set on next update

    samples_.push_back(sample);
    last_timestamp_ns_ = timestamp_ns;
}

double TWAPCalculator::get_twap() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (samples_.empty()) {
        return 0.0;
    }

    // Calculate time-weighted average
    double weighted_sum = 0.0;
    uint64_t total_duration = 0;

    for (const auto& sample : samples_) {
        weighted_sum += sample.price * sample.duration_ns;
        total_duration += sample.duration_ns;
    }

    if (total_duration == 0) {
        // Only one sample or very recent update
        return samples_.back().price;
    }

    return weighted_sum / total_duration;
}

void TWAPCalculator::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
    last_timestamp_ns_ = 0;
}

void TWAPCalculator::remove_expired(uint64_t current_time_ns) {
    uint64_t cutoff = current_time_ns - window_ns_;

    while (!samples_.empty() && samples_.front().timestamp_ns < cutoff) {
        samples_.pop_front();
    }
}

// OrderFlowImbalance implementation

OrderFlowImbalance::OrderFlowImbalance(uint64_t window_ns)
    : window_ns_(window_ns) {
}

void OrderFlowImbalance::update(const OrderBookSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t timestamp_ns = get_timestamp_ns();

    // Remove expired flows
    remove_expired(timestamp_ns);

    // Calculate total bid and ask volumes
    double bid_volume = 0.0;
    double ask_volume = 0.0;

    for (const auto& bid : snapshot.bids) {
        bid_volume += bid.quantity;
    }

    for (const auto& ask : snapshot.asks) {
        ask_volume += ask.quantity;
    }

    // Add new flow point
    FlowPoint flow;
    flow.bid_volume = bid_volume;
    flow.ask_volume = ask_volume;
    flow.timestamp_ns = timestamp_ns;

    flows_.push_back(flow);

    // Update cumulative volumes
    cumulative_bid_volume_ += bid_volume;
    cumulative_ask_volume_ += ask_volume;
}

double OrderFlowImbalance::get_imbalance() const {
    std::lock_guard<std::mutex> lock(mutex_);

    double total = cumulative_bid_volume_ + cumulative_ask_volume_;

    if (total == 0.0) {
        return 0.0;
    }

    return (cumulative_bid_volume_ - cumulative_ask_volume_) / total;
}

double OrderFlowImbalance::get_bid_volume() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cumulative_bid_volume_;
}

double OrderFlowImbalance::get_ask_volume() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cumulative_ask_volume_;
}

void OrderFlowImbalance::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    flows_.clear();
    cumulative_bid_volume_ = 0.0;
    cumulative_ask_volume_ = 0.0;
}

void OrderFlowImbalance::remove_expired(uint64_t current_time_ns) {
    uint64_t cutoff = current_time_ns - window_ns_;

    while (!flows_.empty() && flows_.front().timestamp_ns < cutoff) {
        const auto& flow = flows_.front();
        cumulative_bid_volume_ -= flow.bid_volume;
        cumulative_ask_volume_ -= flow.ask_volume;
        flows_.pop_front();
    }
}

// SpreadAnalyzer implementation

SpreadAnalyzer::SpreadAnalyzer(uint64_t window_ns)
    : window_ns_(window_ns) {
}

void SpreadAnalyzer::update(double bid, double ask, uint64_t timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove expired samples
    remove_expired(timestamp_ns);

    // Calculate spread
    double mid = (bid + ask) / 2.0;
    double spread = ask - bid;
    double spread_bps = (mid > 0.0) ? (spread / mid) * 10000.0 : 0.0;

    // Add sample
    SpreadSample sample;
    sample.bid = bid;
    sample.ask = ask;
    sample.mid = mid;
    sample.spread = spread;
    sample.spread_bps = spread_bps;
    sample.timestamp_ns = timestamp_ns;

    samples_.push_back(sample);
}

SpreadAnalyzer::SpreadStats SpreadAnalyzer::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calculate_stats();
}

void SpreadAnalyzer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
}

void SpreadAnalyzer::remove_expired(uint64_t current_time_ns) {
    uint64_t cutoff = current_time_ns - window_ns_;

    while (!samples_.empty() && samples_.front().timestamp_ns < cutoff) {
        samples_.pop_front();
    }
}

SpreadAnalyzer::SpreadStats SpreadAnalyzer::calculate_stats() const {
    SpreadStats stats;

    if (samples_.empty()) {
        return stats;
    }

    // Current values
    const auto& current = samples_.back();
    stats.current_spread = current.spread;
    stats.current_spread_bps = current.spread_bps;
    stats.sample_count = samples_.size();

    // Calculate average
    double sum_spread = 0.0;
    double sum_spread_bps = 0.0;
    double min_spread = std::numeric_limits<double>::max();
    double max_spread = std::numeric_limits<double>::lowest();

    for (const auto& sample : samples_) {
        sum_spread += sample.spread;
        sum_spread_bps += sample.spread_bps;
        min_spread = std::min(min_spread, sample.spread);
        max_spread = std::max(max_spread, sample.spread);
    }

    stats.average_spread = sum_spread / samples_.size();
    stats.average_spread_bps = sum_spread_bps / samples_.size();
    stats.min_spread = min_spread;
    stats.max_spread = max_spread;

    // Calculate standard deviation
    double variance = 0.0;
    for (const auto& sample : samples_) {
        double diff = sample.spread - stats.average_spread;
        variance += diff * diff;
    }
    stats.std_dev_spread = std::sqrt(variance / samples_.size());

    return stats;
}

// MarketAnalytics implementation

MarketAnalytics::MarketAnalytics(const std::string& symbol)
    : symbol_(symbol)
    , vwap_(std::make_unique<VWAPCalculator>())
    , twap_(std::make_unique<TWAPCalculator>())
    , imbalance_(std::make_unique<OrderFlowImbalance>())
    , spread_(std::make_unique<SpreadAnalyzer>()) {

    spdlog::debug("MarketAnalytics: Created for {}", symbol_);
}

void MarketAnalytics::on_orderbook_update(const OrderBookSnapshot& snapshot) {
    uint64_t timestamp_ns = get_timestamp_ns();

    // Update order flow imbalance
    imbalance_->update(snapshot);

    // Update TWAP with mid price
    if (!snapshot.bids.empty() && !snapshot.asks.empty()) {
        double mid = (snapshot.bids[0].price + snapshot.asks[0].price) / 2.0;
        twap_->update(mid, timestamp_ns);

        // Update spread analyzer
        spread_->update(snapshot.bids[0].price, snapshot.asks[0].price, timestamp_ns);
    }
}

void MarketAnalytics::on_trade(double price, double volume, uint64_t timestamp_ns) {
    // Update VWAP
    vwap_->update(price, volume, timestamp_ns);
}

void MarketAnalytics::reset() {
    vwap_->reset();
    twap_->reset();
    imbalance_->reset();
    spread_->reset();
}

} // namespace marketdata
