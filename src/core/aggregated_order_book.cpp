/**
 * @file aggregated_order_book.cpp
 * @brief Aggregated order book implementation
 */

#include "core/aggregated_order_book.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>

namespace marketdata {

AggregatedOrderBook::AggregatedOrderBook(const std::string& symbol)
    : symbol_(symbol) {
    spdlog::debug("AggregatedOrderBook: Created for {}", symbol_);
}

void AggregatedOrderBook::update(const NormalizedOrderBookUpdate& update) {
    if (update.symbol != symbol_) {
        spdlog::warn("AggregatedOrderBook: Symbol mismatch: {} != {}",
                    update.symbol, symbol_);
        return;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto& book = exchange_books_[update.exchange];
    book.last_update_ns = update.timestamp;

    // Update bids
    for (const auto& bid : update.bids) {
        if (bid.quantity > 0.0) {
            // Add or update level
            AggregatedPriceLevel level;
            level.exchange = update.exchange;
            level.price = bid.price;
            level.quantity = bid.quantity;
            level.order_count = bid.order_count;
            level.timestamp_ns = update.timestamp;

            book.bids[bid.price] = level;
        } else {
            // Remove level (quantity = 0)
            book.bids.erase(bid.price);
        }
    }

    // Update asks
    for (const auto& ask : update.asks) {
        if (ask.quantity > 0.0) {
            AggregatedPriceLevel level;
            level.exchange = update.exchange;
            level.price = ask.price;
            level.quantity = ask.quantity;
            level.order_count = ask.order_count;
            level.timestamp_ns = update.timestamp;

            book.asks[ask.price] = level;
        } else {
            // Remove level
            book.asks.erase(ask.price);
        }
    }

    // Check for arbitrage after update (while holding lock)
    if (arbitrage_callback_) {
        auto arb = detect_arbitrage();
        if (arb) {
            // Release lock before callback
            lock.unlock();
            arbitrage_callback_(*arb);
        }
    }
}

AggregatedSnapshot AggregatedOrderBook::get_snapshot(size_t depth) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    AggregatedSnapshot snapshot;
    snapshot.symbol = symbol_;
    snapshot.timestamp_ns = get_timestamp_ns();
    snapshot.bids = merge_bids(depth);
    snapshot.asks = merge_asks(depth);

    return snapshot;
}

std::optional<ArbitrageOpportunity> AggregatedOrderBook::detect_arbitrage(
    double min_profit_percentage) const {

    std::shared_lock<std::shared_mutex> lock(mutex_);

    // Find best bid and best ask across all exchanges
    std::optional<AggregatedPriceLevel> best_bid;
    std::optional<AggregatedPriceLevel> best_ask;

    for (const auto& [exchange, book] : exchange_books_) {
        // Check bids
        if (!book.bids.empty()) {
            const auto& top_bid = book.bids.rbegin()->second;  // Highest price
            if (!best_bid || top_bid.price > best_bid->price) {
                best_bid = top_bid;
            }
        }

        // Check asks
        if (!book.asks.empty()) {
            const auto& top_ask = book.asks.begin()->second;  // Lowest price
            if (!best_ask || top_ask.price < best_ask->price) {
                best_ask = top_ask;
            }
        }
    }

    if (!best_bid || !best_ask) {
        return std::nullopt;
    }

    // Check if best bid > best ask (crossed market = arbitrage)
    if (best_bid->price <= best_ask->price) {
        return std::nullopt;
    }

    // Calculate profit
    double profit_per_unit = best_bid->price - best_ask->price;
    double profit_percentage = (profit_per_unit / best_ask->price) * 100.0;

    if (profit_percentage < min_profit_percentage) {
        return std::nullopt;
    }

    // Determine max tradeable quantity
    double max_quantity = std::min(best_bid->quantity, best_ask->quantity);

    ArbitrageOpportunity opp;
    opp.symbol = symbol_;
    opp.buy_exchange = best_ask->exchange;   // Buy at lower ask
    opp.sell_exchange = best_bid->exchange;  // Sell at higher bid
    opp.buy_price = best_ask->price;
    opp.sell_price = best_bid->price;
    opp.max_quantity = max_quantity;
    opp.profit_per_unit = profit_per_unit;
    opp.profit_percentage = profit_percentage;
    opp.timestamp_ns = get_timestamp_ns();

    return opp;
}

RoutingRecommendation AggregatedOrderBook::recommend_routing(
    Side side, double quantity) const {

    std::shared_lock<std::shared_mutex> lock(mutex_);

    RoutingRecommendation rec;
    rec.symbol = symbol_;
    rec.side = side;
    rec.quantity = quantity;
    rec.timestamp_ns = get_timestamp_ns();

    // Collect relevant levels from all exchanges
    std::vector<AggregatedPriceLevel> levels;

    for (const auto& [exchange, book] : exchange_books_) {
        const auto& level_map = (side == Side::BUY) ? book.asks : book.bids;

        for (const auto& [price, level] : level_map) {
            levels.push_back(level);
        }
    }

    if (levels.empty()) {
        rec.strategy = RoutingRecommendation::Strategy::SINGLE_VENUE;
        rec.primary_exchange = Exchange::BINANCE;  // Default
        rec.expected_price = 0.0;
        return rec;
    }

    // Sort levels by price
    if (side == Side::BUY) {
        // For buy, sort asks ascending (best price first)
        std::sort(levels.begin(), levels.end(),
            [](const auto& a, const auto& b) { return a.price < b.price; });
    } else {
        // For sell, sort bids descending (best price first)
        std::sort(levels.begin(), levels.end(),
            [](const auto& a, const auto& b) { return a.price > b.price; });
    }

    // Check if single venue has enough liquidity
    double remaining_qty = quantity;
    std::map<Exchange, double> allocations;

    for (const auto& level : levels) {
        if (remaining_qty <= 0.0) {
            break;
        }

        double fill_qty = std::min(remaining_qty, level.quantity);
        allocations[level.exchange] += fill_qty;
        remaining_qty -= fill_qty;
    }

    // Determine strategy
    if (allocations.size() == 1) {
        // Single venue can handle the order
        rec.strategy = RoutingRecommendation::Strategy::SINGLE_VENUE;
        rec.primary_exchange = allocations.begin()->first;
        rec.expected_price = levels[0].price;
    } else {
        // Need to split across venues
        rec.strategy = RoutingRecommendation::Strategy::SPLIT_ORDER;
        rec.allocations = allocations;

        // Calculate volume-weighted average price
        double total_cost = 0.0;
        double total_qty = 0.0;

        for (const auto& level : levels) {
            auto it = allocations.find(level.exchange);
            if (it != allocations.end()) {
                double qty = std::min(it->second, level.quantity);
                total_cost += qty * level.price;
                total_qty += qty;
            }
        }

        rec.expected_price = (total_qty > 0.0) ? (total_cost / total_qty) : 0.0;
        rec.primary_exchange = allocations.begin()->first;  // Largest allocation
    }

    return rec;
}

std::optional<AggregatedPriceLevel> AggregatedOrderBook::get_best_bid() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::optional<AggregatedPriceLevel> best_bid;

    for (const auto& [exchange, book] : exchange_books_) {
        if (!book.bids.empty()) {
            const auto& top_bid = book.bids.rbegin()->second;
            if (!best_bid || top_bid.price > best_bid->price) {
                best_bid = top_bid;
            }
        }
    }

    return best_bid;
}

std::optional<AggregatedPriceLevel> AggregatedOrderBook::get_best_ask() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::optional<AggregatedPriceLevel> best_ask;

    for (const auto& [exchange, book] : exchange_books_) {
        if (!book.asks.empty()) {
            const auto& top_ask = book.asks.begin()->second;
            if (!best_ask || top_ask.price < best_ask->price) {
                best_ask = top_ask;
            }
        }
    }

    return best_ask;
}

std::optional<double> AggregatedOrderBook::get_mid_price() const {
    auto bid = get_best_bid();
    auto ask = get_best_ask();

    if (!bid || !ask) {
        return std::nullopt;
    }

    return (bid->price + ask->price) / 2.0;
}

std::optional<double> AggregatedOrderBook::get_spread() const {
    auto bid = get_best_bid();
    auto ask = get_best_ask();

    if (!bid || !ask) {
        return std::nullopt;
    }

    return ask->price - bid->price;
}

void AggregatedOrderBook::set_arbitrage_callback(ArbitrageCallback callback) {
    arbitrage_callback_ = callback;
}

void AggregatedOrderBook::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    exchange_books_.clear();
}

size_t AggregatedOrderBook::get_exchange_count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return exchange_books_.size();
}

std::vector<AggregatedPriceLevel> AggregatedOrderBook::merge_bids(size_t depth) const {
    // Collect all bids from all exchanges
    std::vector<AggregatedPriceLevel> all_bids;

    for (const auto& [exchange, book] : exchange_books_) {
        for (const auto& [price, level] : book.bids) {
            all_bids.push_back(level);
        }
    }

    // Sort by price descending (best bids first)
    std::sort(all_bids.begin(), all_bids.end(),
        [](const auto& a, const auto& b) { return a.price > b.price; });

    // Return top N
    if (all_bids.size() > depth) {
        all_bids.resize(depth);
    }

    return all_bids;
}

std::vector<AggregatedPriceLevel> AggregatedOrderBook::merge_asks(size_t depth) const {
    // Collect all asks from all exchanges
    std::vector<AggregatedPriceLevel> all_asks;

    for (const auto& [exchange, book] : exchange_books_) {
        for (const auto& [price, level] : book.asks) {
            all_asks.push_back(level);
        }
    }

    // Sort by price ascending (best asks first)
    std::sort(all_asks.begin(), all_asks.end(),
        [](const auto& a, const auto& b) { return a.price < b.price; });

    // Return top N
    if (all_asks.size() > depth) {
        all_asks.resize(depth);
    }

    return all_asks;
}

} // namespace marketdata
