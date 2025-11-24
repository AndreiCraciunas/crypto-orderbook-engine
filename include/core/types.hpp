/**
 * @file types.hpp
 * @brief Core data types and structures for the market data handler
 *
 * This file defines the fundamental data structures used throughout the
 * order book engine, including exchange identifiers, price levels, snapshots,
 * and utility functions for fixed-point arithmetic.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <vector>
#include <optional>

namespace marketdata {

/**
 * @brief Exchange identifier enumeration
 *
 * Identifies which cryptocurrency exchange a particular order or
 * order book update originated from.
 */
enum class Exchange : uint8_t {
    UNKNOWN = 0,   ///< Unknown or unspecified exchange
    BINANCE = 1,   ///< Binance exchange
    COINBASE = 2,  ///< Coinbase exchange
    KRAKEN = 3     ///< Kraken exchange
};

/**
 * @brief Order side enumeration
 *
 * Specifies whether an order is on the bid (buy) or ask (sell) side
 * of the order book.
 */
enum class Side : uint8_t {
    BID = 0,  ///< Bid (buy) side
    ASK = 1   ///< Ask (sell) side
};

/**
 * @brief Convert exchange enum to string representation
 *
 * @param exchange Exchange identifier to convert
 * @return const char* String name of the exchange
 *
 * @code
 * Exchange ex = Exchange::BINANCE;
 * const char* name = exchange_to_string(ex);  // Returns "BINANCE"
 * @endcode
 */
inline const char* exchange_to_string(Exchange exchange) {
    switch (exchange) {
        case Exchange::BINANCE: return "BINANCE";
        case Exchange::COINBASE: return "COINBASE";
        case Exchange::KRAKEN: return "KRAKEN";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Atomic price level structure with cache-line alignment
 *
 * Represents a single price level in the order book. All fields are
 * atomic to allow lock-free concurrent access. The structure is aligned
 * to a cache line (64 bytes) to prevent false sharing between CPU cores.
 *
 * @note This structure uses fixed-point arithmetic internally. Use
 *       double_to_fixed() and fixed_to_double() for conversions.
 * @see double_to_fixed()
 * @see fixed_to_double()
 */
struct alignas(64) PriceLevel {
    std::atomic<uint64_t> price;       ///< Price in fixed-point format (8 decimals)
    std::atomic<uint64_t> quantity;    ///< Total quantity at this price level
    std::atomic<uint32_t> order_count; ///< Number of individual orders
    std::atomic<uint64_t> timestamp;   ///< Last update timestamp (nanoseconds since epoch)

    /**
     * @brief Default constructor initializing all fields to zero
     */
    PriceLevel()
        : price(0)
        , quantity(0)
        , order_count(0)
        , timestamp(0) {}

    /**
     * @brief Parameterized constructor for non-atomic initialization
     *
     * @param p Price value
     * @param q Quantity value
     * @param oc Order count (default: 0)
     * @param ts Timestamp (default: 0)
     */
    PriceLevel(uint64_t p, uint64_t q, uint32_t oc = 0, uint64_t ts = 0)
        : price(p)
        , quantity(q)
        , order_count(oc)
        , timestamp(ts) {}
};

/**
 * @brief Non-atomic price level for snapshots
 *
 * A simplified price level structure without atomic operations,
 * used for order book snapshots and read-only operations. Uses
 * double-precision floating point for easier consumption.
 */
struct PriceLevelSnapshot {
    double price;          ///< Price as double-precision float
    double quantity;       ///< Quantity as double-precision float
    uint32_t order_count;  ///< Number of orders at this level

    /**
     * @brief Default constructor
     */
    PriceLevelSnapshot()
        : price(0.0)
        , quantity(0.0)
        , order_count(0) {}

    /**
     * @brief Parameterized constructor
     *
     * @param p Price value
     * @param q Quantity value
     * @param oc Order count (default: 0)
     */
    PriceLevelSnapshot(double p, double q, uint32_t oc = 0)
        : price(p)
        , quantity(q)
        , order_count(oc) {}
};

/**
 * @brief Order book snapshot structure
 *
 * Contains a point-in-time snapshot of an order book, including
 * bids, asks, and metadata. Generated using SeqLock for consistency.
 */
struct OrderBookSnapshot {
    std::string symbol;                      ///< Trading pair symbol (e.g., "BTC-USD")
    Exchange exchange;                       ///< Source exchange
    uint64_t timestamp;                      ///< Snapshot timestamp (nanoseconds)
    uint64_t sequence_number;                ///< Monotonic sequence number
    std::vector<PriceLevelSnapshot> bids;    ///< Bid levels (sorted descending by price)
    std::vector<PriceLevelSnapshot> asks;    ///< Ask levels (sorted ascending by price)

    /**
     * @brief Default constructor
     */
    OrderBookSnapshot()
        : exchange(Exchange::UNKNOWN)
        , timestamp(0)
        , sequence_number(0) {}
};

/**
 * @brief Normalized order book update from exchanges
 *
 * Represents a normalized update message from any exchange.
 * Exchange-specific messages are converted to this format.
 */
struct NormalizedOrderBookUpdate {
    Exchange exchange;                       ///< Source exchange
    std::string symbol;                      ///< Trading pair symbol
    uint64_t timestamp;                      ///< Update timestamp (nanoseconds since epoch)
    uint64_t update_id;                      ///< Exchange-specific update ID

    std::vector<PriceLevelSnapshot> bids;    ///< Updated bid levels
    std::vector<PriceLevelSnapshot> asks;    ///< Updated ask levels

    /**
     * @brief Default constructor
     */
    NormalizedOrderBookUpdate()
        : exchange(Exchange::UNKNOWN)
        , timestamp(0)
        , update_id(0) {}
};

/**
 * @brief Trade information structure
 *
 * Represents a single trade execution on an exchange.
 */
struct Trade {
    Exchange exchange;      ///< Exchange where trade occurred
    std::string symbol;     ///< Trading pair symbol
    uint64_t timestamp;     ///< Trade timestamp (nanoseconds since epoch)
    std::string trade_id;   ///< Exchange-specific trade ID
    double price;           ///< Execution price
    double quantity;        ///< Execution quantity
    bool is_buyer_maker;    ///< True if buyer was maker (passive order)

    /**
     * @brief Default constructor
     */
    Trade()
        : exchange(Exchange::UNKNOWN)
        , timestamp(0)
        , price(0.0)
        , quantity(0.0)
        , is_buyer_maker(false) {}
};

/**
 * @brief Exchange-specific price level for aggregation
 *
 * Used in aggregated order books to track which exchange
 * provided each price level.
 */
struct ExchangeLevel {
    uint64_t price;      ///< Price in fixed-point format
    uint64_t quantity;   ///< Quantity in fixed-point format
    Exchange exchange;   ///< Source exchange
    uint64_t timestamp;  ///< Level timestamp (nanoseconds)

    /**
     * @brief Default constructor
     */
    ExchangeLevel()
        : price(0)
        , quantity(0)
        , exchange(Exchange::UNKNOWN)
        , timestamp(0) {}

    /**
     * @brief Parameterized constructor
     *
     * @param p Price value
     * @param q Quantity value
     * @param ex Source exchange
     * @param ts Timestamp
     */
    ExchangeLevel(uint64_t p, uint64_t q, Exchange ex, uint64_t ts)
        : price(p)
        , quantity(q)
        , exchange(ex)
        , timestamp(ts) {}
};

/**
 * @brief Order book statistics
 *
 * Contains calculated statistics about the current state of an order book,
 * including spread, mid-price, and volume imbalance.
 */
struct OrderBookStatistics {
    double spread;              ///< Bid-ask spread (best_ask - best_bid)
    double mid_price;           ///< Mid price ((best_bid + best_ask) / 2)
    double book_imbalance;      ///< Volume imbalance: (bid_vol - ask_vol) / (bid_vol + ask_vol)
    uint64_t total_bid_volume;  ///< Total volume on bid side (fixed-point)
    uint64_t total_ask_volume;  ///< Total volume on ask side (fixed-point)
    uint32_t bid_depth;         ///< Number of bid price levels
    uint32_t ask_depth;         ///< Number of ask price levels
    uint64_t timestamp;         ///< Timestamp when statistics were calculated (nanoseconds)

    /**
     * @brief Default constructor
     */
    OrderBookStatistics()
        : spread(0.0)
        , mid_price(0.0)
        , book_imbalance(0.0)
        , total_bid_volume(0)
        , total_ask_volume(0)
        , bid_depth(0)
        , ask_depth(0)
        , timestamp(0) {}
};

/**
 * @brief Fixed-point conversion scale factor
 *
 * All prices are stored internally as 64-bit integers with 8 decimal places
 * of precision. This constant defines the scaling factor (10^8 = 100,000,000).
 *
 * This approach avoids floating-point rounding errors and provides exact
 * arithmetic for financial calculations.
 *
 * @code
 * // Price of $123.456789 is stored as 12345678900
 * uint64_t fixed = double_to_fixed(123.456789);
 * @endcode
 */
constexpr uint64_t PRICE_SCALE = 100000000ULL;

/**
 * @brief Convert double to fixed-point representation
 *
 * Converts a floating-point price to a 64-bit integer with 8 decimal
 * places of precision. This allows for exact arithmetic without
 * floating-point rounding errors.
 *
 * @param value Double-precision price value
 * @return uint64_t Fixed-point representation
 *
 * @code
 * double price = 123.456789;
 * uint64_t fixed = double_to_fixed(price);  // 12345678900
 * double back = fixed_to_double(fixed);     // 123.456789
 * @endcode
 *
 * @note Values are truncated, not rounded
 * @warning Negative values are not supported
 * @see fixed_to_double()
 * @see PRICE_SCALE
 */
inline uint64_t double_to_fixed(double value) {
    return static_cast<uint64_t>(value * PRICE_SCALE);
}

/**
 * @brief Convert fixed-point to double representation
 *
 * Converts a 64-bit fixed-point integer back to a double-precision
 * floating-point number.
 *
 * @param value Fixed-point representation
 * @return double Double-precision price value
 *
 * @code
 * uint64_t fixed = 12345678900;
 * double price = fixed_to_double(fixed);  // 123.456789
 * @endcode
 *
 * @see double_to_fixed()
 * @see PRICE_SCALE
 */
inline double fixed_to_double(uint64_t value) {
    return static_cast<double>(value) / PRICE_SCALE;
}

} // namespace marketdata
