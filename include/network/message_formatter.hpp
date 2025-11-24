/**
 * @file message_formatter.hpp
 * @brief Formats backend data to frontend-expected JSON format
 *
 * Converts internal C++ data structures to JSON messages that match
 * the frontend BACKEND_INTEGRATION.md specification.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "core/types.hpp"
#include <string>
#include <sstream>
#include <iomanip>

namespace marketdata {

/**
 * @brief Message formatter for frontend integration
 *
 * Converts backend data structures to JSON strings matching frontend expectations.
 */
class MessageFormatter {
public:
    /**
     * @brief Convert Exchange enum to lowercase string
     *
     * Frontend expects: "binance", "coinbase", "kraken"
     *
     * @param exchange Exchange enum value
     * @return std::string Lowercase exchange name
     */
    static std::string exchange_to_string(Exchange exchange) {
        switch (exchange) {
            case Exchange::BINANCE: return "binance";
            case Exchange::COINBASE: return "coinbase";
            case Exchange::KRAKEN: return "kraken";
            default: return "unknown";
        }
    }

    /**
     * @brief Convert Side enum to lowercase string
     *
     * Frontend expects: "buy" or "sell"
     *
     * @param side Side enum value
     * @return std::string Lowercase side name
     */
    static std::string side_to_string(Side side) {
        return (side == Side::BID) ? "buy" : "sell";
    }

    /**
     * @brief Format order book update message
     *
     * Creates JSON message matching frontend order_book_update format
     *
     * @param update Normalized order book update
     * @param sequence Sequence number (default: 0)
     * @return std::string JSON message
     */
    static std::string format_order_book_update(
        const NormalizedOrderBookUpdate& update,
        uint64_t sequence = 0
    ) {
        std::ostringstream json;
        json << std::fixed << std::setprecision(8);

        json << "{"
             << "\"type\":\"order_book_update\","
             << "\"exchange\":\"" << exchange_to_string(update.exchange) << "\","
             << "\"symbol\":\"" << update.symbol << "\","
             << "\"timestamp\":" << update.timestamp << ","
             << "\"sequence\":" << sequence << ","
             << "\"bids\":[";

        // Format bids (use PriceLevelSnapshot structure)
        for (size_t i = 0; i < update.bids.size(); ++i) {
            if (i > 0) json << ",";
            json << "[" << update.bids[i].price << ","
                 << update.bids[i].quantity;
            if (update.bids[i].order_count > 0) {
                json << "," << update.bids[i].order_count;
            }
            json << "]";
        }

        json << "],\"asks\":[";

        // Format asks (use PriceLevelSnapshot structure)
        for (size_t i = 0; i < update.asks.size(); ++i) {
            if (i > 0) json << ",";
            json << "[" << update.asks[i].price << ","
                 << update.asks[i].quantity;
            if (update.asks[i].order_count > 0) {
                json << "," << update.asks[i].order_count;
            }
            json << "]";
        }

        json << "]}";
        return json.str();
    }

    /**
     * @brief Format trade message
     *
     * Creates JSON message matching frontend trade format
     *
     * @param trade Trade data
     * @return std::string JSON message
     */
    static std::string format_trade(const Trade& trade) {
        std::ostringstream json;
        json << std::fixed << std::setprecision(8);

        // Derive side from is_buyer_maker: if buyer was maker, trade was a sell
        std::string side = trade.is_buyer_maker ? "sell" : "buy";

        json << "{"
             << "\"type\":\"trade\","
             << "\"exchange\":\"" << exchange_to_string(trade.exchange) << "\","
             << "\"symbol\":\"" << trade.symbol << "\","
             << "\"price\":" << trade.price << ","
             << "\"quantity\":" << trade.quantity << ","
             << "\"timestamp\":" << trade.timestamp << ","
             << "\"side\":\"" << side << "\"";

        if (!trade.trade_id.empty()) {
            json << ",\"id\":\"" << trade.trade_id << "\"";
        }

        json << ",\"isBuyerMaker\":" << (trade.is_buyer_maker ? "true" : "false");
        json << "}";

        return json.str();
    }

    /**
     * @brief Format metrics message
     *
     * Creates JSON message matching frontend metrics format
     *
     * @param latency_p50 50th percentile latency (ns)
     * @param latency_p95 95th percentile latency (ns)
     * @param latency_p99 99th percentile latency (ns)
     * @param latency_p999 99.9th percentile latency (ns)
     * @param messages_per_sec Messages per second
     * @param bytes_per_sec Bytes per second
     * @param status Connection status
     * @param reconnect_attempts Reconnection attempts
     * @return std::string JSON message
     */
    static std::string format_metrics(
        uint64_t latency_p50,
        uint64_t latency_p95,
        uint64_t latency_p99,
        uint64_t latency_p999,
        uint64_t messages_per_sec,
        uint64_t bytes_per_sec,
        const std::string& status = "connected",
        uint32_t reconnect_attempts = 0
    ) {
        std::ostringstream json;

        json << "{"
             << "\"type\":\"metrics\","
             << "\"latency\":{"
             << "\"p50\":" << latency_p50 << ","
             << "\"p95\":" << latency_p95 << ","
             << "\"p99\":" << latency_p99 << ","
             << "\"p999\":" << latency_p999
             << "},"
             << "\"throughput\":{"
             << "\"messagesPerSecond\":" << messages_per_sec << ","
             << "\"bytesPerSecond\":" << bytes_per_sec
             << "},"
             << "\"websocket\":{"
             << "\"status\":\"" << status << "\","
             << "\"reconnectAttempts\":" << reconnect_attempts
             << "}"
             << "}";

        return json.str();
    }

    /**
     * @brief Format market statistics message
     *
     * Creates JSON message matching frontend stats format
     *
     * @param symbol Trading pair symbol
     * @param last_price Most recent trade price
     * @param price_change_24h 24h price change (absolute)
     * @param price_change_pct_24h 24h price change (percentage)
     * @param high_24h 24h high price
     * @param low_24h 24h low price
     * @param volume_24h 24h base volume
     * @param quote_volume_24h 24h quote volume
     * @param timestamp Unix timestamp
     * @return std::string JSON message
     */
    static std::string format_stats(
        const std::string& symbol,
        double last_price,
        double price_change_24h,
        double price_change_pct_24h,
        double high_24h,
        double low_24h,
        double volume_24h,
        double quote_volume_24h,
        uint64_t timestamp
    ) {
        std::ostringstream json;
        json << std::fixed << std::setprecision(8);

        json << "{"
             << "\"type\":\"stats\","
             << "\"symbol\":\"" << symbol << "\","
             << "\"lastPrice\":" << last_price << ","
             << "\"priceChange24h\":" << price_change_24h << ","
             << "\"priceChangePercent24h\":" << price_change_pct_24h << ","
             << "\"high24h\":" << high_24h << ","
             << "\"low24h\":" << low_24h << ","
             << "\"volume24h\":" << volume_24h << ","
             << "\"quoteVolume24h\":" << quote_volume_24h << ","
             << "\"timestamp\":" << timestamp
             << "}";

        return json.str();
    }
};

} // namespace marketdata
