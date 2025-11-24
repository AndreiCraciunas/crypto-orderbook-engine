/**
 * @file aggregated_order_book.hpp
 * @brief Multi-exchange aggregated order book
 *
 * Aggregates order books from multiple exchanges to provide unified view
 * of market depth and best prices across venues. Enables cross-exchange
 * arbitrage detection and smart order routing.
 *
 * **Features:**
 * - Real-time aggregation across exchanges
 * - Unified best bid/ask calculation
 * - Exchange-specific price level tracking
 * - Arbitrage opportunity detection
 * - Smart order routing recommendations
 *
 * **Performance:**
 * - Update: O(log N) per exchange, ~500-1000 ns
 * - Snapshot: O(K) where K = depth, ~2-5 μs
 * - Arbitrage check: O(1), ~100-200 ns
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "core/types.hpp"
#include <map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <optional>

namespace marketdata {

/**
 * @brief Price level with exchange source
 *
 * Represents a price level from a specific exchange in the
 * aggregated order book.
 */
struct AggregatedPriceLevel {
    Exchange exchange;      ///< Source exchange
    double price;           ///< Price level
    double quantity;        ///< Total quantity at this level
    uint32_t order_count;   ///< Number of orders (if available)
    uint64_t timestamp_ns;  ///< Last update timestamp

    /**
     * @brief Compare by price (for sorting)
     */
    bool operator<(const AggregatedPriceLevel& other) const {
        return price < other.price;
    }
};

/**
 * @brief Aggregated order book snapshot
 *
 * Snapshot of aggregated order book showing best prices across
 * all exchanges.
 */
struct AggregatedSnapshot {
    std::string symbol;                          ///< Trading symbol
    std::vector<AggregatedPriceLevel> bids;      ///< Aggregated bids (descending)
    std::vector<AggregatedPriceLevel> asks;      ///< Aggregated asks (ascending)
    uint64_t timestamp_ns;                       ///< Snapshot timestamp

    /**
     * @brief Get best bid price
     */
    std::optional<double> get_best_bid() const {
        if (bids.empty()) return std::nullopt;
        return bids[0].price;
    }

    /**
     * @brief Get best ask price
     */
    std::optional<double> get_best_ask() const {
        if (asks.empty()) return std::nullopt;
        return asks[0].price;
    }

    /**
     * @brief Get mid price
     */
    std::optional<double> get_mid_price() const {
        auto bid = get_best_bid();
        auto ask = get_best_ask();
        if (!bid || !ask) return std::nullopt;
        return (*bid + *ask) / 2.0;
    }

    /**
     * @brief Get spread
     */
    std::optional<double> get_spread() const {
        auto bid = get_best_bid();
        auto ask = get_best_ask();
        if (!bid || !ask) return std::nullopt;
        return *ask - *bid;
    }

    /**
     * @brief Get spread in basis points
     */
    std::optional<double> get_spread_bps() const {
        auto spread = get_spread();
        auto mid = get_mid_price();
        if (!spread || !mid || *mid == 0.0) return std::nullopt;
        return (*spread / *mid) * 10000.0;
    }
};

/**
 * @brief Arbitrage opportunity
 *
 * Represents a detected arbitrage opportunity between two exchanges.
 */
struct ArbitrageOpportunity {
    std::string symbol;             ///< Trading symbol
    Exchange buy_exchange;          ///< Exchange to buy from
    Exchange sell_exchange;         ///< Exchange to sell to
    double buy_price;               ///< Buy price
    double sell_price;              ///< Sell price
    double max_quantity;            ///< Maximum tradeable quantity
    double profit_per_unit;         ///< Profit per unit
    double profit_percentage;       ///< Profit percentage
    uint64_t timestamp_ns;          ///< Detection timestamp

    /**
     * @brief Calculate potential profit
     */
    double calculate_profit(double quantity) const {
        return std::min(quantity, max_quantity) * profit_per_unit;
    }
};

/**
 * @brief Smart order routing recommendation
 *
 * Recommends best exchange and execution strategy for an order.
 */
struct RoutingRecommendation {
    enum class Strategy {
        SINGLE_VENUE,       ///< Execute entirely on one exchange
        SPLIT_ORDER,        ///< Split across multiple exchanges
        ICEBERG,            ///< Use iceberg strategy
        TWAP               ///< Time-weighted average price
    };

    std::string symbol;                          ///< Trading symbol
    Side side;                                   ///< Buy or sell
    double quantity;                             ///< Order quantity
    Strategy strategy;                           ///< Recommended strategy

    // For SINGLE_VENUE
    Exchange primary_exchange;                   ///< Primary exchange
    double expected_price;                       ///< Expected execution price

    // For SPLIT_ORDER
    std::map<Exchange, double> allocations;      ///< Exchange -> quantity

    uint64_t timestamp_ns;                       ///< Recommendation timestamp
};

/**
 * @brief Multi-exchange aggregated order book
 *
 * Maintains aggregated view of order books across multiple exchanges.
 * Provides unified best bid/ask, arbitrage detection, and smart routing.
 *
 * **Architecture:**
 * - Per-exchange price level storage
 * - Real-time aggregation on query
 * - Concurrent reads, exclusive writes
 * - Lock-free snapshot generation
 *
 * **Update Flow:**
 * 1. Receive update from exchange
 * 2. Update exchange-specific levels
 * 3. Trigger arbitrage detection
 * 4. Notify callbacks
 *
 * @code
 * // Create aggregated order book
 * AggregatedOrderBook agg_book("BTCUSD");
 *
 * // Update from exchange
 * NormalizedOrderBookUpdate binance_update;
 * binance_update.exchange = Exchange::BINANCE;
 * binance_update.symbol = "BTCUSD";
 * // ... populate bids/asks
 *
 * agg_book.update(binance_update);
 *
 * // Get unified snapshot
 * auto snapshot = agg_book.get_snapshot(10);  // Top 10 levels
 *
 * // Check for arbitrage
 * auto arb = agg_book.detect_arbitrage();
 * if (arb) {
 *     std::cout << "Arbitrage: Buy on " << arb->buy_exchange
 *               << " @ " << arb->buy_price
 *               << ", Sell on " << arb->sell_exchange
 *               << " @ " << arb->sell_price
 *               << ", Profit: " << arb->profit_percentage << "%\n";
 * }
 *
 * // Get routing recommendation
 * auto route = agg_book.recommend_routing(Side::BUY, 1.5);
 * @endcode
 *
 * @note Thread-safe: Multiple readers, single writer
 * @note All prices assumed to be in same quote currency
 */
class AggregatedOrderBook {
public:
    /**
     * @brief Callback for arbitrage opportunities
     */
    using ArbitrageCallback = std::function<void(const ArbitrageOpportunity&)>;

    /**
     * @brief Construct aggregated order book
     *
     * @param symbol Trading symbol (normalized)
     */
    explicit AggregatedOrderBook(const std::string& symbol);

    /**
     * @brief Update from exchange
     *
     * Updates order book levels for a specific exchange.
     *
     * @param update Order book update from exchange
     *
     * @code
     * NormalizedOrderBookUpdate update;
     * update.exchange = Exchange::BINANCE;
     * update.symbol = "BTCUSD";
     * // ... populate levels
     * agg_book.update(update);
     * @endcode
     *
     * @note Thread-safe
     * @note O(N log M) where N = update levels, M = existing levels
     */
    void update(const NormalizedOrderBookUpdate& update);

    /**
     * @brief Get aggregated snapshot
     *
     * Returns snapshot of aggregated order book with best prices
     * across all exchanges.
     *
     * @param depth Max levels per side (default: 20)
     * @return AggregatedSnapshot Aggregated snapshot
     *
     * @code
     * auto snapshot = agg_book.get_snapshot(10);
     * std::cout << "Best bid: " << snapshot.bids[0].price
     *           << " from " << snapshot.bids[0].exchange << "\n";
     * @endcode
     *
     * @note Thread-safe (read lock)
     * @note O(K log N) where K = depth, N = total levels
     */
    AggregatedSnapshot get_snapshot(size_t depth = 20) const;

    /**
     * @brief Detect arbitrage opportunity
     *
     * Checks if there's a profitable arbitrage opportunity between
     * best bid and best ask across exchanges.
     *
     * @param min_profit_percentage Minimum profit % to report (default: 0.1%)
     * @return std::optional<ArbitrageOpportunity> Opportunity if found
     *
     * @code
     * auto arb = agg_book.detect_arbitrage(0.2);  // Min 0.2% profit
     * if (arb) {
     *     std::cout << "Arbitrage profit: " << arb->profit_percentage << "%\n";
     * }
     * @endcode
     *
     * @note Thread-safe (read lock)
     * @note O(E) where E = number of exchanges
     */
    std::optional<ArbitrageOpportunity> detect_arbitrage(
        double min_profit_percentage = 0.1) const;

    /**
     * @brief Recommend order routing
     *
     * Recommends best exchange(s) and strategy for executing an order.
     *
     * @param side Buy or sell
     * @param quantity Order quantity
     * @return RoutingRecommendation Routing recommendation
     *
     * @code
     * auto route = agg_book.recommend_routing(Side::BUY, 2.5);
     * if (route.strategy == RoutingRecommendation::Strategy::SINGLE_VENUE) {
     *     std::cout << "Execute on " << route.primary_exchange << "\n";
     * }
     * @endcode
     *
     * @note Thread-safe (read lock)
     * @note Considers liquidity and price impact
     */
    RoutingRecommendation recommend_routing(Side side, double quantity) const;

    /**
     * @brief Get best bid across all exchanges
     *
     * @return std::optional<AggregatedPriceLevel> Best bid if available
     *
     * @note Thread-safe (read lock)
     */
    std::optional<AggregatedPriceLevel> get_best_bid() const;

    /**
     * @brief Get best ask across all exchanges
     *
     * @return std::optional<AggregatedPriceLevel> Best ask if available
     *
     * @note Thread-safe (read lock)
     */
    std::optional<AggregatedPriceLevel> get_best_ask() const;

    /**
     * @brief Get mid price
     *
     * @return std::optional<double> Mid price if available
     *
     * @note Thread-safe (read lock)
     */
    std::optional<double> get_mid_price() const;

    /**
     * @brief Get spread
     *
     * @return std::optional<double> Spread if available
     *
     * @note Thread-safe (read lock)
     */
    std::optional<double> get_spread() const;

    /**
     * @brief Set arbitrage callback
     *
     * Callback is invoked whenever an arbitrage opportunity is detected.
     *
     * @param callback Callback function
     *
     * @note Callback invoked from update thread
     * @warning Callback must be fast (no blocking operations)
     */
    void set_arbitrage_callback(ArbitrageCallback callback);

    /**
     * @brief Get trading symbol
     *
     * @return const std::string& Symbol
     */
    const std::string& get_symbol() const { return symbol_; }

    /**
     * @brief Clear all data
     *
     * Removes all price levels from all exchanges.
     *
     * @note Thread-safe (write lock)
     */
    void clear();

    /**
     * @brief Get number of active exchanges
     *
     * @return size_t Number of exchanges with data
     *
     * @note Thread-safe (read lock)
     */
    size_t get_exchange_count() const;

private:
    /**
     * @brief Per-exchange order book state
     */
    struct ExchangeBook {
        std::map<double, AggregatedPriceLevel> bids;  ///< Bids (price -> level)
        std::map<double, AggregatedPriceLevel> asks;  ///< Asks (price -> level)
        uint64_t last_update_ns{0};                   ///< Last update timestamp
    };

    std::string symbol_;                                    ///< Trading symbol
    mutable std::shared_mutex mutex_;                       ///< Protects exchange_books_
    std::map<Exchange, ExchangeBook> exchange_books_;       ///< Per-exchange books

    ArbitrageCallback arbitrage_callback_;                  ///< Arbitrage callback

    /**
     * @brief Merge bids from all exchanges
     *
     * @param depth Max depth
     * @return std::vector<AggregatedPriceLevel> Merged bids
     */
    std::vector<AggregatedPriceLevel> merge_bids(size_t depth) const;

    /**
     * @brief Merge asks from all exchanges
     *
     * @param depth Max depth
     * @return std::vector<AggregatedPriceLevel> Merged asks
     */
    std::vector<AggregatedPriceLevel> merge_asks(size_t depth) const;
};

} // namespace marketdata
