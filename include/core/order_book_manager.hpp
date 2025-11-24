/**
 * @file order_book_manager.hpp
 * @brief Multi-symbol order book management and aggregation
 *
 * Provides centralized management for multiple order books across different
 * symbols and exchanges. Supports callbacks for real-time updates and
 * cross-exchange aggregation.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "core/order_book.hpp"
#include "core/types.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <string>
#include <memory>
#include <functional>

namespace marketdata {

/**
 * @brief Callback function type for order book updates
 *
 * Invoked whenever an order book is updated. Receives symbol and
 * current snapshot for processing.
 *
 * @param symbol Trading symbol (e.g., "BTC-USDT")
 * @param snapshot Current order book snapshot after update
 */
using OrderBookUpdateCallback = std::function<void(const std::string& symbol,
                                                     const OrderBookSnapshot& snapshot)>;

/**
 * @brief Manages multiple order books for different symbols and exchanges
 *
 * Thread-safe manager for creating, updating, and querying multiple order books.
 * Uses shared_mutex for efficient concurrent read access while maintaining
 * safety for write operations.
 *
 * **Features:**
 * - Multi-symbol order book management
 * - Thread-safe concurrent access (readers-writer lock)
 * - Real-time update callbacks
 * - Snapshot and statistics queries
 * - Dynamic order book creation/removal
 *
 * **Thread Safety:**
 * - Multiple concurrent readers supported
 * - Write operations (create/remove) acquire exclusive lock
 * - Update operations use shared lock (order books are lock-free)
 *
 * @code
 * OrderBookManager manager;
 *
 * // Create order books
 * manager.create_order_book("BTC-USDT");
 * manager.create_order_book("ETH-USDT");
 *
 * // Set callback for updates
 * manager.set_update_callback([](const std::string& symbol,
 *                                  const OrderBookSnapshot& snapshot) {
 *     std::cout << symbol << " updated, spread: "
 *               << (snapshot.asks[0].price - snapshot.bids[0].price) << '\n';
 * });
 *
 * // Process update
 * NormalizedOrderBookUpdate update;
 * update.symbol = "BTC-USDT";
 * update.bids.push_back({50000.0, 1.5, 3});
 * manager.process_update(update);
 *
 * // Query snapshot
 * auto snapshot = manager.get_snapshot("BTC-USDT", 20);
 * if (snapshot) {
 *     std::cout << "Best bid: " << snapshot->bids[0].price << '\n';
 * }
 * @endcode
 *
 * @note Thread-safe for all operations
 * @note Order books are created on-demand
 * @see OrderBook for individual order book implementation
 * @see AggregatedOrderBook for cross-exchange aggregation
 */
class OrderBookManager {
public:
    static constexpr size_t DEFAULT_MAX_LEVELS = 1000;  ///< Default max levels per side

    /**
     * @brief Construct empty manager
     */
    OrderBookManager() = default;

    /**
     * @brief Destructor
     */
    ~OrderBookManager() = default;

    // Non-copyable
    OrderBookManager(const OrderBookManager&) = delete;
    OrderBookManager& operator=(const OrderBookManager&) = delete;

    /**
     * @brief Create order book for a symbol
     *
     * Creates a new order book for the specified symbol. Returns false
     * if order book already exists.
     *
     * @param symbol Trading symbol (e.g., "BTC-USDT", "ETH-USDT")
     * @param max_levels Maximum price levels per side (default: 1000)
     * @return bool True if created, false if already exists
     *
     * @code
     * OrderBookManager manager;
     *
     * // Create standard order book
     * if (manager.create_order_book("BTC-USDT")) {
     *     std::cout << "Created BTC-USDT order book\n";
     * }
     *
     * // Create with custom capacity
     * manager.create_order_book("ETH-USDT", 500);
     *
     * // Duplicate fails
     * if (!manager.create_order_book("BTC-USDT")) {
     *     std::cout << "BTC-USDT already exists\n";
     * }
     * @endcode
     *
     * @note Thread-safe - acquires exclusive lock
     * @see remove_order_book()
     */
    bool create_order_book(const std::string& symbol, size_t max_levels = DEFAULT_MAX_LEVELS) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        if (order_books_.find(symbol) != order_books_.end()) {
            return false; // Already exists
        }

        order_books_[symbol] = std::make_unique<OrderBook<>>();
        return true;
    }

    /**
     * @brief Process order book update
     *
     * Applies batch update to order book and triggers callback if set.
     * Update contains bid and ask levels to add/update/remove.
     *
     * @param update Normalized update with symbol, exchange, levels
     * @return bool True if processed, false if order book doesn't exist
     *
     * @code
     * OrderBookManager manager;
     * manager.create_order_book("BTC-USDT");
     *
     * // Create update
     * NormalizedOrderBookUpdate update;
     * update.symbol = "BTC-USDT";
     * update.exchange = Exchange::BINANCE;
     * update.bids.push_back({50000.0, 1.5, 3});
     * update.asks.push_back({50100.0, 2.0, 5});
     *
     * // Process update
     * if (manager.process_update(update)) {
     *     std::cout << "Update applied\n";
     * }
     * @endcode
     *
     * @note Thread-safe - uses shared lock (reads map, order book is lock-free)
     * @note Triggers update callback if registered
     * @note Quantity = 0 removes price level
     * @see set_update_callback()
     */
    bool process_update(const NormalizedOrderBookUpdate& update) {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        auto it = order_books_.find(update.symbol);
        if (it == order_books_.end()) {
            return false; // Order book doesn't exist
        }

        auto& book = it->second;

        // Update bids
        for (const auto& bid : update.bids) {
            book->update_bid(bid.price, bid.quantity, bid.order_count);
        }

        // Update asks
        for (const auto& ask : update.asks) {
            book->update_ask(ask.price, ask.quantity, ask.order_count);
        }

        // Notify callbacks
        if (update_callback_) {
            auto snapshot = book->get_snapshot();
            snapshot.symbol = update.symbol;
            snapshot.exchange = update.exchange;
            update_callback_(update.symbol, snapshot);
        }

        return true;
    }

    /**
     * @brief Get order book snapshot
     *
     * Returns consistent snapshot of top N levels for a symbol.
     *
     * @param symbol Trading symbol
     * @param depth Number of levels per side (default: 20)
     * @return std::optional<OrderBookSnapshot> Snapshot if symbol exists
     *
     * @code
     * OrderBookManager manager;
     * manager.create_order_book("BTC-USDT");
     * // ... populate book ...
     *
     * // Get top 10 levels
     * auto snapshot = manager.get_snapshot("BTC-USDT", 10);
     * if (snapshot) {
     *     std::cout << "Bids: " << snapshot->bids.size() << '\n';
     *     std::cout << "Best bid: " << snapshot->bids[0].price << '\n';
     * }
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     * @note Returns std::nullopt if symbol not found
     * @see get_statistics()
     */
    std::optional<OrderBookSnapshot> get_snapshot(const std::string& symbol,
                                                   size_t depth = 20) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        auto it = order_books_.find(symbol);
        if (it == order_books_.end()) {
            return std::nullopt;
        }

        auto snapshot = it->second->get_snapshot(depth);
        snapshot.symbol = symbol;
        return snapshot;
    }

    /**
     * @brief Get order book statistics
     *
     * Returns comprehensive statistics (spread, mid, volumes, depths).
     *
     * @param symbol Trading symbol
     * @return std::optional<OrderBookStatistics> Stats if symbol exists
     *
     * @code
     * auto stats = manager.get_statistics("BTC-USDT");
     * if (stats) {
     *     std::cout << "Spread: " << stats->spread << '\n';
     *     std::cout << "Mid: " << stats->mid_price << '\n';
     *     std::cout << "Bid depth: " << stats->bid_depth << " levels\n";
     * }
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     * @note Returns std::nullopt if symbol not found
     * @see OrderBookStatistics
     */
    std::optional<OrderBookStatistics> get_statistics(const std::string& symbol) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        auto it = order_books_.find(symbol);
        if (it == order_books_.end()) {
            return std::nullopt;
        }

        return it->second->get_statistics();
    }

    /**
     * @brief Get bid-ask spread
     *
     * Returns spread (best_ask - best_bid) for a symbol.
     *
     * @param symbol Trading symbol
     * @return std::optional<double> Spread if symbol exists
     *
     * @code
     * auto spread = manager.get_spread("BTC-USDT");
     * if (spread) {
     *     std::cout << "Spread: $" << *spread << '\n';
     * }
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     * @note Returns std::nullopt if symbol not found
     */
    std::optional<double> get_spread(const std::string& symbol) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        auto it = order_books_.find(symbol);
        if (it == order_books_.end()) {
            return std::nullopt;
        }

        return it->second->get_spread();
    }

    /**
     * @brief Get mid price
     *
     * Returns mid price (average of best bid and ask) for a symbol.
     *
     * @param symbol Trading symbol
     * @return std::optional<double> Mid price if symbol exists
     *
     * @code
     * auto mid = manager.get_mid_price("BTC-USDT");
     * if (mid) {
     *     std::cout << "Mid price: $" << *mid << '\n';
     * }
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     * @note Returns std::nullopt if symbol not found
     */
    std::optional<double> get_mid_price(const std::string& symbol) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        auto it = order_books_.find(symbol);
        if (it == order_books_.end()) {
            return std::nullopt;
        }

        return it->second->get_mid_price();
    }

    /**
     * @brief Clear order book
     *
     * Removes all levels from order book but keeps it in manager.
     *
     * @param symbol Trading symbol
     * @return bool True if cleared, false if symbol not found
     *
     * @code
     * manager.clear_order_book("BTC-USDT");
     * // Order book still exists but is empty
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     * @see remove_order_book() to delete order book entirely
     */
    bool clear_order_book(const std::string& symbol) {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        auto it = order_books_.find(symbol);
        if (it == order_books_.end()) {
            return false;
        }

        it->second->clear();
        return true;
    }

    /**
     * @brief Remove order book
     *
     * Deletes order book for a symbol completely from manager.
     *
     * @param symbol Trading symbol
     * @return bool True if removed, false if not found
     *
     * @code
     * manager.remove_order_book("BTC-USDT");
     * // Order book no longer exists
     * @endcode
     *
     * @note Thread-safe - acquires exclusive lock
     * @see create_order_book() to recreate
     * @see clear_order_book() to empty without removing
     */
    bool remove_order_book(const std::string& symbol) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        auto it = order_books_.find(symbol);
        if (it == order_books_.end()) {
            return false;
        }

        order_books_.erase(it);
        return true;
    }

    /**
     * @brief Get list of all symbols
     *
     * Returns vector of all symbol names currently managed.
     *
     * @return std::vector<std::string> List of symbol names
     *
     * @code
     * OrderBookManager manager;
     * manager.create_order_book("BTC-USDT");
     * manager.create_order_book("ETH-USDT");
     *
     * auto symbols = manager.get_symbols();
     * for (const auto& symbol : symbols) {
     *     std::cout << symbol << '\n';
     * }
     * // Output: BTC-USDT, ETH-USDT (order not guaranteed)
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     * @note Order of symbols is not guaranteed
     */
    std::vector<std::string> get_symbols() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        std::vector<std::string> symbols;
        symbols.reserve(order_books_.size());

        for (const auto& [symbol, _] : order_books_) {
            symbols.push_back(symbol);
        }

        return symbols;
    }

    /**
     * @brief Set update callback
     *
     * Registers callback function to be invoked on every order book update.
     * Callback receives symbol and current snapshot.
     *
     * @param callback Function to call on updates
     *
     * @code
     * OrderBookManager manager;
     * manager.create_order_book("BTC-USDT");
     *
     * // Register callback
     * manager.set_update_callback([](const std::string& symbol,
     *                                  const OrderBookSnapshot& snapshot) {
     *     std::cout << symbol << " updated at " << snapshot.timestamp << '\n';
     *     std::cout << "Best bid: " << snapshot.bids[0].price << '\n';
     *     std::cout << "Best ask: " << snapshot.asks[0].price << '\n';
     * });
     *
     * // Now all updates trigger callback
     * NormalizedOrderBookUpdate update;
     * update.symbol = "BTC-USDT";
     * // ... populate update ...
     * manager.process_update(update);  // Callback invoked
     * @endcode
     *
     * @note Not thread-safe - set before processing updates
     * @note Callback is invoked synchronously during update
     * @warning Callback should be fast to avoid blocking updates
     * @see process_update()
     */
    void set_update_callback(OrderBookUpdateCallback callback) {
        update_callback_ = std::move(callback);
    }

    /**
     * @brief Get number of order books
     *
     * Returns count of currently managed order books.
     *
     * @return size_t Number of order books
     *
     * @code
     * OrderBookManager manager;
     * std::cout << "Books: " << manager.size() << '\n';  // 0
     *
     * manager.create_order_book("BTC-USDT");
     * std::cout << "Books: " << manager.size() << '\n';  // 1
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     */
    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return order_books_.size();
    }

private:
    mutable std::shared_mutex mutex_;  ///< Reader-writer lock for thread safety
    std::unordered_map<std::string, std::unique_ptr<OrderBook<>>> order_books_;  ///< Symbol -> OrderBook map
    OrderBookUpdateCallback update_callback_;  ///< Optional update callback
};

/**
 * @brief Aggregated order book combining multiple exchanges
 *
 * Maintains a unified order book view across multiple exchanges for the
 * same trading pair. Tracks which exchange provides each price level.
 *
 * **Features:**
 * - Cross-exchange price level aggregation
 * - Best bid/ask across all exchanges
 * - Per-exchange level tracking
 * - Weighted mid price calculation
 *
 * **Use Cases:**
 * - Arbitrage opportunity detection
 * - Best execution price discovery
 * - Market depth analysis across venues
 *
 * @code
 * AggregatedOrderBook agg_book;
 *
 * // Update from Binance
 * agg_book.update_level(Exchange::BINANCE, Side::BID, 50000.0, 1.5);
 * agg_book.update_level(Exchange::BINANCE, Side::ASK, 50100.0, 2.0);
 *
 * // Update from Coinbase
 * agg_book.update_level(Exchange::COINBASE, Side::BID, 50010.0, 1.0);
 * agg_book.update_level(Exchange::COINBASE, Side::ASK, 50090.0, 0.5);
 *
 * // Get best bid across exchanges
 * auto best_bid = agg_book.get_best_bid();
 * if (best_bid) {
 *     std::cout << "Best bid: " << fixed_to_double(best_bid->price)
 *               << " on " << exchange_to_string(best_bid->exchange) << '\n';
 *     // Output: Best bid: 50010.0 on COINBASE
 * }
 *
 * // Get aggregated snapshot
 * auto snapshot = agg_book.get_snapshot(10);
 * std::cout << "Total bid levels: " << snapshot.bids.size() << '\n';
 * @endcode
 *
 * @note Thread-safe for concurrent access
 * @note Levels sorted by price (bids desc, asks asc)
 * @see OrderBookManager for single-exchange management
 */
class AggregatedOrderBook {
public:
    /**
     * @brief Aggregated price level
     *
     * Represents a price level with contributions from multiple exchanges.
     */
    struct AggregatedLevel {
        double price;                               ///< Price level
        double quantity;                            ///< Total quantity across exchanges
        std::vector<ExchangeLevel> exchange_levels; ///< Per-exchange breakdown

        /**
         * @brief Default constructor
         */
        AggregatedLevel() : price(0.0), quantity(0.0) {}

        /**
         * @brief Construct aggregated level
         *
         * @param p Price
         * @param q Total quantity
         */
        AggregatedLevel(double p, double q)
            : price(p), quantity(q) {}
    };

    /**
     * @brief Aggregated snapshot
     *
     * Complete view of aggregated order book across exchanges.
     */
    struct AggregatedSnapshot {
        std::string symbol;                 ///< Trading symbol
        uint64_t timestamp;                 ///< Snapshot timestamp
        std::vector<AggregatedLevel> bids;  ///< Aggregated bid levels
        std::vector<AggregatedLevel> asks;  ///< Aggregated ask levels
    };

    /**
     * @brief Construct empty aggregated order book
     */
    AggregatedOrderBook() = default;

    /**
     * @brief Update level from specific exchange
     *
     * Adds, updates, or removes a price level from a specific exchange.
     * Maintains sorted order automatically.
     *
     * @param exchange Exchange providing the level
     * @param side Bid or ask side
     * @param price Price level
     * @param quantity Quantity (0 to remove)
     *
     * @code
     * AggregatedOrderBook book;
     *
     * // Add bid from Binance
     * book.update_level(Exchange::BINANCE, Side::BID, 50000.0, 1.5);
     *
     * // Update same level
     * book.update_level(Exchange::BINANCE, Side::BID, 50000.0, 2.0);
     *
     * // Remove level (quantity = 0)
     * book.update_level(Exchange::BINANCE, Side::BID, 50000.0, 0.0);
     * @endcode
     *
     * @note Thread-safe - acquires exclusive lock
     * @note Automatically sorts levels after update
     * @note Quantity = 0 removes level from that exchange
     */
    void update_level(Exchange exchange, Side side, double price, double quantity) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        uint64_t price_fixed = double_to_fixed(price);
        uint64_t quantity_fixed = double_to_fixed(quantity);
        uint64_t timestamp = get_timestamp_ns();

        ExchangeLevel level(price_fixed, quantity_fixed, exchange, timestamp);

        auto& levels = (side == Side::BID) ? aggregated_bids_ : aggregated_asks_;

        // Find existing price level
        auto it = std::find_if(levels.begin(), levels.end(),
                               [price_fixed](const ExchangeLevel& l) {
                                   return l.price == price_fixed;
                               });

        if (quantity_fixed == 0) {
            // Remove level if quantity is zero
            if (it != levels.end() && it->exchange == exchange) {
                levels.erase(it);
            }
        } else {
            if (it != levels.end() && it->exchange == exchange) {
                // Update existing level
                it->quantity = quantity_fixed;
                it->timestamp = timestamp;
            } else {
                // Add new level
                levels.push_back(level);

                // Sort: bids descending, asks ascending
                if (side == Side::BID) {
                    std::sort(levels.begin(), levels.end(),
                             [](const ExchangeLevel& a, const ExchangeLevel& b) {
                                 return a.price > b.price;
                             });
                } else {
                    std::sort(levels.begin(), levels.end(),
                             [](const ExchangeLevel& a, const ExchangeLevel& b) {
                                 return a.price < b.price;
                             });
                }
            }
        }
    }

    /**
     * @brief Get aggregated snapshot
     *
     * Returns snapshot with levels grouped by price, showing total
     * quantity and per-exchange breakdown.
     *
     * @param depth Number of levels per side (default: 20)
     * @return AggregatedSnapshot Snapshot with exchange breakdown
     *
     * @code
     * AggregatedOrderBook book;
     * // ... populate from multiple exchanges ...
     *
     * auto snapshot = book.get_snapshot(10);
     * for (const auto& bid : snapshot.bids) {
     *     std::cout << "Price: " << bid.price
     *               << ", Total qty: " << bid.quantity << '\n';
     *
     *     // Show per-exchange breakdown
     *     for (const auto& ex_level : bid.exchange_levels) {
     *         std::cout << "  " << exchange_to_string(ex_level.exchange)
     *                   << ": " << fixed_to_double(ex_level.quantity) << '\n';
     *     }
     * }
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     * @note Groups levels by price across exchanges
     * @note Limited to specified depth per side
     */
    AggregatedSnapshot get_snapshot(size_t depth = 20) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        AggregatedSnapshot snapshot;
        snapshot.timestamp = get_timestamp_ns();

        // Aggregate bids by price level
        std::unordered_map<uint64_t, std::vector<ExchangeLevel>> bid_map;
        for (const auto& level : aggregated_bids_) {
            bid_map[level.price].push_back(level);
        }

        for (const auto& [price, levels] : bid_map) {
            double total_qty = 0.0;
            for (const auto& level : levels) {
                total_qty += fixed_to_double(level.quantity);
            }

            AggregatedLevel agg_level(fixed_to_double(price), total_qty);
            agg_level.exchange_levels = levels;
            snapshot.bids.push_back(agg_level);
        }

        // Sort bids descending
        std::sort(snapshot.bids.begin(), snapshot.bids.end(),
                 [](const AggregatedLevel& a, const AggregatedLevel& b) {
                     return a.price > b.price;
                 });

        // Limit to depth
        if (snapshot.bids.size() > depth) {
            snapshot.bids.resize(depth);
        }

        // Aggregate asks by price level
        std::unordered_map<uint64_t, std::vector<ExchangeLevel>> ask_map;
        for (const auto& level : aggregated_asks_) {
            ask_map[level.price].push_back(level);
        }

        for (const auto& [price, levels] : ask_map) {
            double total_qty = 0.0;
            for (const auto& level : levels) {
                total_qty += fixed_to_double(level.quantity);
            }

            AggregatedLevel agg_level(fixed_to_double(price), total_qty);
            agg_level.exchange_levels = levels;
            snapshot.asks.push_back(agg_level);
        }

        // Sort asks ascending
        std::sort(snapshot.asks.begin(), snapshot.asks.end(),
                 [](const AggregatedLevel& a, const AggregatedLevel& b) {
                     return a.price < b.price;
                 });

        // Limit to depth
        if (snapshot.asks.size() > depth) {
            snapshot.asks.resize(depth);
        }

        return snapshot;
    }

    /**
     * @brief Get best bid across all exchanges
     *
     * Returns the highest bid price level from any exchange.
     *
     * @return std::optional<ExchangeLevel> Best bid if exists
     *
     * @code
     * auto best = agg_book.get_best_bid();
     * if (best) {
     *     std::cout << "Best bid: " << fixed_to_double(best->price)
     *               << " from " << exchange_to_string(best->exchange) << '\n';
     * }
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     * @note Returns std::nullopt if no bids
     */
    std::optional<ExchangeLevel> get_best_bid() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        if (aggregated_bids_.empty()) {
            return std::nullopt;
        }

        return aggregated_bids_.front();
    }

    /**
     * @brief Get best ask across all exchanges
     *
     * Returns the lowest ask price level from any exchange.
     *
     * @return std::optional<ExchangeLevel> Best ask if exists
     *
     * @code
     * auto best = agg_book.get_best_ask();
     * if (best) {
     *     std::cout << "Best ask: " << fixed_to_double(best->price)
     *               << " from " << exchange_to_string(best->exchange) << '\n';
     * }
     * @endcode
     *
     * @note Thread-safe - uses shared lock
     * @note Returns std::nullopt if no asks
     */
    std::optional<ExchangeLevel> get_best_ask() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        if (aggregated_asks_.empty()) {
            return std::nullopt;
        }

        return aggregated_asks_.front();
    }

    /**
     * @brief Get weighted mid price
     *
     * Calculates mid price from best bid and ask across all exchanges.
     *
     * @return double Mid price, or 0.0 if book empty
     *
     * @code
     * double mid = agg_book.get_weighted_mid_price();
     * std::cout << "Global mid price: $" << mid << '\n';
     * @endcode
     *
     * @note Returns 0.0 if either side is empty
     */
    double get_weighted_mid_price() const {
        auto best_bid = get_best_bid();
        auto best_ask = get_best_ask();

        if (!best_bid || !best_ask) {
            return 0.0;
        }

        double bid_price = fixed_to_double(best_bid->price);
        double ask_price = fixed_to_double(best_ask->price);

        return (bid_price + ask_price) / 2.0;
    }

    /**
     * @brief Clear all levels
     *
     * Removes all bid and ask levels from all exchanges.
     *
     * @code
     * agg_book.clear();
     * // Now empty
     * @endcode
     *
     * @note Thread-safe - acquires exclusive lock
     */
    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        aggregated_bids_.clear();
        aggregated_asks_.clear();
    }

private:
    mutable std::shared_mutex mutex_;           ///< Reader-writer lock
    std::vector<ExchangeLevel> aggregated_bids_;  ///< All bid levels (sorted desc)
    std::vector<ExchangeLevel> aggregated_asks_;  ///< All ask levels (sorted asc)
};

} // namespace marketdata
