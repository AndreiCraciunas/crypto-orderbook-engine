/**
 * @file test_aggregated_order_book.cpp
 * @brief Unit tests for AggregatedOrderBook class
 */

#include <gtest/gtest.h>
#include "core/aggregated_order_book.hpp"

using namespace marketdata;

class AggregatedOrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        agg_book = std::make_unique<AggregatedOrderBook>("BTCUSD");
    }

    NormalizedOrderBookUpdate create_update(Exchange exchange, double bid_price, double bid_qty,
                                            double ask_price, double ask_qty) {
        NormalizedOrderBookUpdate update;
        update.symbol = "BTCUSD";
        update.exchange = exchange;
        update.timestamp_ns = 1000000000;
        update.is_snapshot = false;

        if (bid_qty > 0) {
            OrderBookLevel bid;
            bid.price = bid_price;
            bid.quantity = bid_qty;
            bid.order_count = 1;
            update.bids.push_back(bid);
        }

        if (ask_qty > 0) {
            OrderBookLevel ask;
            ask.price = ask_price;
            ask.quantity = ask_qty;
            ask.order_count = 1;
            update.asks.push_back(ask);
        }

        return update;
    }

    std::unique_ptr<AggregatedOrderBook> agg_book;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(AggregatedOrderBookTest, InitialState) {
    auto snapshot = agg_book->get_snapshot(10);

    EXPECT_EQ(snapshot.symbol, "BTCUSD");
    EXPECT_TRUE(snapshot.bids.empty());
    EXPECT_TRUE(snapshot.asks.empty());
}

TEST_F(AggregatedOrderBookTest, SingleExchangeUpdate) {
    auto update = create_update(Exchange::BINANCE, 42000.0, 1.0, 42001.0, 1.5);
    agg_book->update(update);

    auto snapshot = agg_book->get_snapshot(10);

    ASSERT_EQ(snapshot.bids.size(), 1);
    ASSERT_EQ(snapshot.asks.size(), 1);

    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 42000.0);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].quantity, 1.0);
    EXPECT_EQ(snapshot.bids[0].exchange, Exchange::BINANCE);

    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 42001.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[0].quantity, 1.5);
    EXPECT_EQ(snapshot.asks[0].exchange, Exchange::BINANCE);
}

TEST_F(AggregatedOrderBookTest, MultiExchangeUpdate) {
    // Binance: bid 42000, ask 42002
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 42002.0, 1.5);
    agg_book->update(update1);

    // Coinbase: bid 42001 (better), ask 42003
    auto update2 = create_update(Exchange::COINBASE, 42001.0, 2.0, 42003.0, 2.5);
    agg_book->update(update2);

    auto snapshot = agg_book->get_snapshot(10);

    ASSERT_GE(snapshot.bids.size(), 2);
    ASSERT_GE(snapshot.asks.size(), 2);

    // Best bid should be Coinbase at 42001
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 42001.0);
    EXPECT_EQ(snapshot.bids[0].exchange, Exchange::COINBASE);

    // Best ask should be Binance at 42002
    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 42002.0);
    EXPECT_EQ(snapshot.asks[0].exchange, Exchange::BINANCE);
}

// ============================================================================
// Aggregation Logic
// ============================================================================

TEST_F(AggregatedOrderBookTest, BidAggregationBestFirst) {
    // Add bids from different exchanges
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 0, 0);
    auto update2 = create_update(Exchange::COINBASE, 42001.0, 2.0, 0, 0);  // Better
    auto update3 = create_update(Exchange::KRAKEN, 41999.0, 1.5, 0, 0);   // Worse

    agg_book->update(update1);
    agg_book->update(update2);
    agg_book->update(update3);

    auto snapshot = agg_book->get_snapshot(10);

    ASSERT_GE(snapshot.bids.size(), 3);

    // Should be sorted descending (best first)
    EXPECT_GE(snapshot.bids[0].price, snapshot.bids[1].price);
    EXPECT_GE(snapshot.bids[1].price, snapshot.bids[2].price);

    // Best bid should be Coinbase
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 42001.0);
}

TEST_F(AggregatedOrderBookTest, AskAggregationBestFirst) {
    // Add asks from different exchanges
    auto update1 = create_update(Exchange::BINANCE, 0, 0, 42002.0, 1.0);
    auto update2 = create_update(Exchange::COINBASE, 0, 0, 42001.0, 2.0);  // Better
    auto update3 = create_update(Exchange::KRAKEN, 0, 0, 42003.0, 1.5);   // Worse

    agg_book->update(update1);
    agg_book->update(update2);
    agg_book->update(update3);

    auto snapshot = agg_book->get_snapshot(10);

    ASSERT_GE(snapshot.asks.size(), 3);

    // Should be sorted ascending (best first)
    EXPECT_LE(snapshot.asks[0].price, snapshot.asks[1].price);
    EXPECT_LE(snapshot.asks[1].price, snapshot.asks[2].price);

    // Best ask should be Coinbase
    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 42001.0);
}

// ============================================================================
// Arbitrage Detection
// ============================================================================

TEST_F(AggregatedOrderBookTest, ArbitrageOpportunity) {
    // Binance: bid 42000, ask 42100
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 42100.0, 1.0);
    agg_book->update(update1);

    // Coinbase: bid 42200, ask 42300 (arbitrage possible)
    auto update2 = create_update(Exchange::COINBASE, 42200.0, 1.0, 42300.0, 1.0);
    agg_book->update(update2);

    // Buy on Binance @ 42100, sell on Coinbase @ 42200
    auto arb = agg_book->detect_arbitrage(0.1);

    ASSERT_TRUE(arb.has_value());
    EXPECT_EQ(arb->buy_exchange, Exchange::BINANCE);
    EXPECT_EQ(arb->sell_exchange, Exchange::COINBASE);
    EXPECT_DOUBLE_EQ(arb->buy_price, 42100.0);
    EXPECT_DOUBLE_EQ(arb->sell_price, 42200.0);
    EXPECT_GT(arb->profit_percentage, 0.1);
}

TEST_F(AggregatedOrderBookTest, NoArbitrageOpportunity) {
    // Binance: bid 42000, ask 42001
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 42001.0, 1.0);
    agg_book->update(update1);

    // Coinbase: bid 41999, ask 42002 (no arbitrage)
    auto update2 = create_update(Exchange::COINBASE, 41999.0, 1.0, 42002.0, 1.0);
    agg_book->update(update2);

    auto arb = agg_book->detect_arbitrage(0.1);

    EXPECT_FALSE(arb.has_value());
}

TEST_F(AggregatedOrderBookTest, ArbitrageProfitThreshold) {
    // Small profit opportunity
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 42010.0, 1.0);
    auto update2 = create_update(Exchange::COINBASE, 42020.0, 1.0, 42030.0, 1.0);

    agg_book->update(update1);
    agg_book->update(update2);

    // Should detect with low threshold
    auto arb1 = agg_book->detect_arbitrage(0.01);
    EXPECT_TRUE(arb1.has_value());

    // Should NOT detect with high threshold
    auto arb2 = agg_book->detect_arbitrage(5.0);
    EXPECT_FALSE(arb2.has_value());
}

// ============================================================================
// Smart Order Routing
// ============================================================================

TEST_F(AggregatedOrderBookTest, SmartRoutingBuy) {
    // Binance: ask 42000 @ 1.0
    auto update1 = create_update(Exchange::BINANCE, 0, 0, 42000.0, 1.0);
    agg_book->update(update1);

    // Coinbase: ask 42001 @ 2.0
    auto update2 = create_update(Exchange::COINBASE, 0, 0, 42001.0, 2.0);
    agg_book->update(update2);

    // Buy 1.5 BTC
    auto routing = agg_book->recommend_routing(Side::BUY, 1.5);

    ASSERT_EQ(routing.legs.size(), 2);

    // First leg: Binance (best price, 1.0 BTC)
    EXPECT_EQ(routing.legs[0].exchange, Exchange::BINANCE);
    EXPECT_DOUBLE_EQ(routing.legs[0].price, 42000.0);
    EXPECT_DOUBLE_EQ(routing.legs[0].quantity, 1.0);

    // Second leg: Coinbase (0.5 BTC remaining)
    EXPECT_EQ(routing.legs[1].exchange, Exchange::COINBASE);
    EXPECT_DOUBLE_EQ(routing.legs[1].price, 42001.0);
    EXPECT_DOUBLE_EQ(routing.legs[1].quantity, 0.5);

    // Check total cost
    double expected_cost = (1.0 * 42000.0) + (0.5 * 42001.0);
    EXPECT_DOUBLE_EQ(routing.total_cost, expected_cost);
}

TEST_F(AggregatedOrderBookTest, SmartRoutingSell) {
    // Binance: bid 42000 @ 1.0
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 0, 0);
    agg_book->update(update1);

    // Coinbase: bid 41999 @ 2.0
    auto update2 = create_update(Exchange::COINBASE, 41999.0, 2.0, 0, 0);
    agg_book->update(update2);

    // Sell 1.5 BTC
    auto routing = agg_book->recommend_routing(Side::SELL, 1.5);

    ASSERT_EQ(routing.legs.size(), 2);

    // First leg: Binance (best price, 1.0 BTC)
    EXPECT_EQ(routing.legs[0].exchange, Exchange::BINANCE);
    EXPECT_DOUBLE_EQ(routing.legs[0].price, 42000.0);
    EXPECT_DOUBLE_EQ(routing.legs[0].quantity, 1.0);

    // Second leg: Coinbase (0.5 BTC remaining)
    EXPECT_EQ(routing.legs[1].exchange, Exchange::COINBASE);
    EXPECT_DOUBLE_EQ(routing.legs[1].price, 41999.0);
    EXPECT_DOUBLE_EQ(routing.legs[1].quantity, 0.5);
}

TEST_F(AggregatedOrderBookTest, SmartRoutingInsufficientLiquidity) {
    // Only 1.0 BTC available
    auto update = create_update(Exchange::BINANCE, 0, 0, 42000.0, 1.0);
    agg_book->update(update);

    // Try to buy 2.0 BTC
    auto routing = agg_book->recommend_routing(Side::BUY, 2.0);

    ASSERT_EQ(routing.legs.size(), 1);
    EXPECT_DOUBLE_EQ(routing.legs[0].quantity, 1.0);  // Only fills what's available
    EXPECT_LT(routing.total_quantity, 2.0);  // Didn't fill full amount
}

// ============================================================================
// Spread Calculation
// ============================================================================

TEST_F(AggregatedOrderBookTest, GlobalSpread) {
    // Binance: bid 42000, ask 42002
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 42002.0, 1.0);
    agg_book->update(update1);

    // Coinbase: bid 41999, ask 42003
    auto update2 = create_update(Exchange::COINBASE, 41999.0, 1.0, 42003.0, 1.0);
    agg_book->update(update2);

    auto snapshot = agg_book->get_snapshot(10);

    // Global best bid: 42000 (Binance)
    // Global best ask: 42002 (Binance)
    // Global spread: 2.0

    auto spread = snapshot.get_spread();
    ASSERT_TRUE(spread.has_value());
    EXPECT_DOUBLE_EQ(*spread, 2.0);
}

TEST_F(AggregatedOrderBookTest, TightestSpreadAcrossExchanges) {
    // Binance: wide spread (42000-42010 = 10)
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 42010.0, 1.0);
    agg_book->update(update1);

    // Coinbase: tighter spread (42005-42006 = 1)
    auto update2 = create_update(Exchange::COINBASE, 42005.0, 1.0, 42006.0, 1.0);
    agg_book->update(update2);

    auto snapshot = agg_book->get_snapshot(10);

    // Global best bid: 42005 (Coinbase)
    // Global best ask: 42006 (Coinbase)
    // Global spread: 1.0

    auto spread = snapshot.get_spread();
    ASSERT_TRUE(spread.has_value());
    EXPECT_DOUBLE_EQ(*spread, 1.0);
}

// ============================================================================
// Snapshot Updates
// ============================================================================

TEST_F(AggregatedOrderBookTest, SnapshotReplacement) {
    // Initial update
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 42001.0, 1.0);
    update1.is_snapshot = false;
    agg_book->update(update1);

    // Snapshot update (should replace)
    auto update2 = create_update(Exchange::BINANCE, 42100.0, 2.0, 42101.0, 2.0);
    update2.is_snapshot = true;
    agg_book->update(update2);

    auto snapshot = agg_book->get_snapshot(10);

    // Should have new prices from snapshot
    bool found_new_bid = false;
    for (const auto& bid : snapshot.bids) {
        if (bid.exchange == Exchange::BINANCE && bid.price == 42100.0) {
            found_new_bid = true;
            break;
        }
    }
    EXPECT_TRUE(found_new_bid);
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(AggregatedOrderBookTest, PerformanceMultiExchangeUpdate) {
    const int num_updates = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_updates; i++) {
        Exchange ex = static_cast<Exchange>(i % 3);  // Rotate through exchanges
        auto update = create_update(ex, 42000.0 + i, 1.0, 42001.0 + i, 1.0);
        agg_book->update(update);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_updates;

    // Should be sub-microsecond
    EXPECT_LT(avg_ns, 2000.0) << "Average update time: " << avg_ns << " ns";

    std::cout << "Average aggregated update time: " << avg_ns << " ns\n";
}

TEST_F(AggregatedOrderBookTest, PerformanceArbitrageDetection) {
    // Setup order books
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 1.0, 42100.0, 1.0);
    auto update2 = create_update(Exchange::COINBASE, 42200.0, 1.0, 42300.0, 1.0);
    agg_book->update(update1);
    agg_book->update(update2);

    const int num_checks = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_checks; i++) {
        auto arb = agg_book->detect_arbitrage(0.1);
        volatile bool found = arb.has_value();
        (void)found;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_checks;

    // Should be very fast (< 500 ns)
    EXPECT_LT(avg_ns, 500.0) << "Average arbitrage detection time: " << avg_ns << " ns";

    std::cout << "Average arbitrage detection time: " << avg_ns << " ns\n";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
