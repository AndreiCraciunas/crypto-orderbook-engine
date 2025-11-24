/**
 * @file test_analytics.cpp
 * @brief Unit tests for analytics components (VWAP, TWAP, Imbalance, Spread)
 */

#include <gtest/gtest.h>
#include "analytics/market_analytics.hpp"
#include <cmath>

using namespace marketdata;

// ============================================================================
// VWAP Tests
// ============================================================================

class VWAPTest : public ::testing::Test {
protected:
    void SetUp() override {
        vwap = std::make_unique<VWAPCalculator>(60'000'000'000);  // 60 second window
    }

    std::unique_ptr<VWAPCalculator> vwap;
};

TEST_F(VWAPTest, InitialState) {
    EXPECT_DOUBLE_EQ(vwap->get_vwap(), 0.0);
}

TEST_F(VWAPTest, SingleTrade) {
    vwap->update(42000.0, 1.5, 1000000000);

    EXPECT_DOUBLE_EQ(vwap->get_vwap(), 42000.0);
}

TEST_F(VWAPTest, MultipleTrades) {
    uint64_t timestamp = 1000000000;

    // Trade 1: 42000 @ 1.0
    vwap->update(42000.0, 1.0, timestamp);

    // Trade 2: 42100 @ 2.0
    vwap->update(42100.0, 2.0, timestamp + 1000000);

    // VWAP = (42000*1.0 + 42100*2.0) / (1.0 + 2.0)
    //      = (42000 + 84200) / 3.0
    //      = 126200 / 3.0
    //      = 42066.67

    EXPECT_NEAR(vwap->get_vwap(), 42066.67, 0.01);
}

TEST_F(VWAPTest, WindowExpiration) {
    uint64_t timestamp = 1000000000;

    // Trade 1
    vwap->update(42000.0, 1.0, timestamp);

    // Trade 2 (61 seconds later - outside window)
    vwap->update(42100.0, 2.0, timestamp + 61'000'000'000);

    // Only trade 2 should be in window
    EXPECT_DOUBLE_EQ(vwap->get_vwap(), 42100.0);
}

TEST_F(VWAPTest, ZeroVolumeIgnored) {
    vwap->update(42000.0, 1.0, 1000000000);
    vwap->update(99999.0, 0.0, 1000000000);  // Should be ignored

    EXPECT_DOUBLE_EQ(vwap->get_vwap(), 42000.0);
}

// ============================================================================
// TWAP Tests
// ============================================================================

class TWAPTest : public ::testing::Test {
protected:
    void SetUp() override {
        twap = std::make_unique<TWAPCalculator>(300'000'000'000);  // 5 minute window
    }

    std::unique_ptr<TWAPCalculator> twap;
};

TEST_F(TWAPTest, InitialState) {
    EXPECT_DOUBLE_EQ(twap->get_twap(), 0.0);
}

TEST_F(TWAPTest, SinglePrice) {
    twap->update(42000.0, 1000000000);

    EXPECT_DOUBLE_EQ(twap->get_twap(), 42000.0);
}

TEST_F(TWAPTest, MultiplePrices) {
    uint64_t timestamp = 1000000000;

    twap->update(42000.0, timestamp);
    twap->update(42100.0, timestamp + 1000000000);
    twap->update(42200.0, timestamp + 2000000000);

    // Simple average: (42000 + 42100 + 42200) / 3 = 42100
    EXPECT_DOUBLE_EQ(twap->get_twap(), 42100.0);
}

TEST_F(TWAPTest, WindowExpiration) {
    uint64_t timestamp = 1000000000;

    twap->update(42000.0, timestamp);
    twap->update(42100.0, timestamp + 301'000'000'000);  // 301 seconds later

    // Only second price in window
    EXPECT_DOUBLE_EQ(twap->get_twap(), 42100.0);
}

// ============================================================================
// Order Flow Imbalance Tests
// ============================================================================

class OrderFlowImbalanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        imbalance = std::make_unique<OrderFlowImbalance>(10'000'000'000);  // 10 second window
    }

    OrderBookSnapshot create_snapshot(double bid_vol, double ask_vol) {
        OrderBookSnapshot snapshot;
        snapshot.symbol = "BTCUSD";
        snapshot.exchange = Exchange::BINANCE;
        snapshot.timestamp_ns = 1000000000;

        if (bid_vol > 0) {
            OrderBookLevel bid;
            bid.price = 42000.0;
            bid.quantity = bid_vol;
            bid.order_count = 1;
            snapshot.bids.push_back(bid);
        }

        if (ask_vol > 0) {
            OrderBookLevel ask;
            ask.price = 42001.0;
            ask.quantity = ask_vol;
            ask.order_count = 1;
            snapshot.asks.push_back(ask);
        }

        return snapshot;
    }

    std::unique_ptr<OrderFlowImbalance> imbalance;
};

TEST_F(OrderFlowImbalanceTest, InitialState) {
    EXPECT_DOUBLE_EQ(imbalance->get_imbalance(), 0.0);
}

TEST_F(OrderFlowImbalanceTest, BalancedBook) {
    auto snapshot = create_snapshot(1.0, 1.0);  // Equal bid/ask volume
    imbalance->update(snapshot);

    EXPECT_DOUBLE_EQ(imbalance->get_imbalance(), 0.0);
}

TEST_F(OrderFlowImbalanceTest, BuyPressure) {
    auto snapshot = create_snapshot(3.0, 1.0);  // More bid volume
    imbalance->update(snapshot);

    // Imbalance = (3.0 - 1.0) / (3.0 + 1.0) = 2.0 / 4.0 = 0.5
    EXPECT_DOUBLE_EQ(imbalance->get_imbalance(), 0.5);
}

TEST_F(OrderFlowImbalanceTest, SellPressure) {
    auto snapshot = create_snapshot(1.0, 3.0);  // More ask volume
    imbalance->update(snapshot);

    // Imbalance = (1.0 - 3.0) / (1.0 + 3.0) = -2.0 / 4.0 = -0.5
    EXPECT_DOUBLE_EQ(imbalance->get_imbalance(), -0.5);
}

TEST_F(OrderFlowImbalanceTest, ExtremeImbalance) {
    auto snapshot = create_snapshot(10.0, 0.01);  // Extreme buy pressure
    imbalance->update(snapshot);

    // Should be close to +1.0
    EXPECT_NEAR(imbalance->get_imbalance(), 1.0, 0.01);
}

TEST_F(OrderFlowImbalanceTest, RangeValidation) {
    // Imbalance should always be in [-1, 1]
    for (double bid_vol = 0.1; bid_vol <= 10.0; bid_vol += 0.5) {
        for (double ask_vol = 0.1; ask_vol <= 10.0; ask_vol += 0.5) {
            auto snapshot = create_snapshot(bid_vol, ask_vol);
            imbalance->update(snapshot);

            double imb = imbalance->get_imbalance();
            EXPECT_GE(imb, -1.0);
            EXPECT_LE(imb, 1.0);
        }
    }
}

// ============================================================================
// Spread Analyzer Tests
// ============================================================================

class SpreadAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override {
        analyzer = std::make_unique<SpreadAnalyzer>(60'000'000'000);  // 60 second window
    }

    OrderBookSnapshot create_snapshot(double bid, double ask, uint64_t timestamp) {
        OrderBookSnapshot snapshot;
        snapshot.symbol = "BTCUSD";
        snapshot.exchange = Exchange::BINANCE;
        snapshot.timestamp_ns = timestamp;

        OrderBookLevel bid_level;
        bid_level.price = bid;
        bid_level.quantity = 1.0;
        bid_level.order_count = 1;
        snapshot.bids.push_back(bid_level);

        OrderBookLevel ask_level;
        ask_level.price = ask;
        ask_level.quantity = 1.0;
        ask_level.order_count = 1;
        snapshot.asks.push_back(ask_level);

        return snapshot;
    }

    std::unique_ptr<SpreadAnalyzer> analyzer;
};

TEST_F(SpreadAnalyzerTest, InitialState) {
    auto stats = analyzer->get_statistics();

    EXPECT_DOUBLE_EQ(stats.current_spread, 0.0);
    EXPECT_DOUBLE_EQ(stats.average_spread, 0.0);
    EXPECT_DOUBLE_EQ(stats.min_spread, 0.0);
    EXPECT_DOUBLE_EQ(stats.max_spread, 0.0);
}

TEST_F(SpreadAnalyzerTest, SingleUpdate) {
    auto snapshot = create_snapshot(42000.0, 42001.0, 1000000000);
    analyzer->update(snapshot);

    auto stats = analyzer->get_statistics();

    EXPECT_DOUBLE_EQ(stats.current_spread, 1.0);
    EXPECT_DOUBLE_EQ(stats.average_spread, 1.0);
    EXPECT_DOUBLE_EQ(stats.min_spread, 1.0);
    EXPECT_DOUBLE_EQ(stats.max_spread, 1.0);
}

TEST_F(SpreadAnalyzerTest, MultipleUpdates) {
    uint64_t timestamp = 1000000000;

    analyzer->update(create_snapshot(42000.0, 42001.0, timestamp));          // spread = 1.0
    analyzer->update(create_snapshot(42000.0, 42002.0, timestamp + 1'000'000'000));  // spread = 2.0
    analyzer->update(create_snapshot(42000.0, 42003.0, timestamp + 2'000'000'000));  // spread = 3.0

    auto stats = analyzer->get_statistics();

    EXPECT_DOUBLE_EQ(stats.current_spread, 3.0);
    EXPECT_DOUBLE_EQ(stats.average_spread, 2.0);  // (1+2+3)/3
    EXPECT_DOUBLE_EQ(stats.min_spread, 1.0);
    EXPECT_DOUBLE_EQ(stats.max_spread, 3.0);
}

TEST_F(SpreadAnalyzerTest, SpreadBPS) {
    auto snapshot = create_snapshot(42000.0, 42001.0, 1000000000);
    analyzer->update(snapshot);

    auto stats = analyzer->get_statistics();

    // Spread BPS = (1.0 / 42000.5) * 10000 ≈ 2.38
    EXPECT_NEAR(stats.current_spread_bps, 2.38, 0.01);
}

TEST_F(SpreadAnalyzerTest, WindowExpiration) {
    uint64_t timestamp = 1000000000;

    analyzer->update(create_snapshot(42000.0, 42001.0, timestamp));          // spread = 1.0
    analyzer->update(create_snapshot(42000.0, 42005.0, timestamp + 61'000'000'000));  // spread = 5.0

    auto stats = analyzer->get_statistics();

    // Old spread should be expired
    EXPECT_DOUBLE_EQ(stats.current_spread, 5.0);
    EXPECT_DOUBLE_EQ(stats.average_spread, 5.0);
    EXPECT_DOUBLE_EQ(stats.min_spread, 5.0);
    EXPECT_DOUBLE_EQ(stats.max_spread, 5.0);
}

// ============================================================================
// MarketAnalytics Integration Tests
// ============================================================================

class MarketAnalyticsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto vwap_calc = std::make_shared<VWAPCalculator>(60'000'000'000);
        auto twap_calc = std::make_shared<TWAPCalculator>(300'000'000'000);
        auto imbalance_calc = std::make_shared<OrderFlowImbalance>(10'000'000'000);
        auto spread_calc = std::make_shared<SpreadAnalyzer>(60'000'000'000);

        analytics = std::make_unique<MarketAnalytics>(
            "BTCUSD", vwap_calc, twap_calc, imbalance_calc, spread_calc
        );
    }

    OrderBookSnapshot create_snapshot(double bid_price, double bid_vol,
                                      double ask_price, double ask_vol,
                                      uint64_t timestamp) {
        OrderBookSnapshot snapshot;
        snapshot.symbol = "BTCUSD";
        snapshot.exchange = Exchange::BINANCE;
        snapshot.timestamp_ns = timestamp;

        OrderBookLevel bid;
        bid.price = bid_price;
        bid.quantity = bid_vol;
        bid.order_count = 1;
        snapshot.bids.push_back(bid);

        OrderBookLevel ask;
        ask.price = ask_price;
        ask.quantity = ask_vol;
        ask.order_count = 1;
        snapshot.asks.push_back(ask);

        return snapshot;
    }

    std::unique_ptr<MarketAnalytics> analytics;
};

TEST_F(MarketAnalyticsTest, InitialState) {
    EXPECT_EQ(analytics->get_symbol(), "BTCUSD");
    EXPECT_DOUBLE_EQ(analytics->get_vwap(), 0.0);
    EXPECT_DOUBLE_EQ(analytics->get_twap(), 0.0);
    EXPECT_DOUBLE_EQ(analytics->get_imbalance(), 0.0);

    auto spread_stats = analytics->get_spread_stats();
    EXPECT_DOUBLE_EQ(spread_stats.current_spread, 0.0);
}

TEST_F(MarketAnalyticsTest, SingleUpdate) {
    auto snapshot = create_snapshot(42000.0, 1.0, 42001.0, 1.0, 1000000000);
    analytics->update(snapshot);

    // VWAP uses mid price
    double mid = 42000.5;
    EXPECT_DOUBLE_EQ(analytics->get_vwap(), mid);
    EXPECT_DOUBLE_EQ(analytics->get_twap(), mid);

    // Balanced book
    EXPECT_DOUBLE_EQ(analytics->get_imbalance(), 0.0);

    // Spread
    auto spread_stats = analytics->get_spread_stats();
    EXPECT_DOUBLE_EQ(spread_stats.current_spread, 1.0);
}

TEST_F(MarketAnalyticsTest, MultipleUpdates) {
    uint64_t timestamp = 1000000000;

    analytics->update(create_snapshot(42000.0, 1.0, 42001.0, 1.0, timestamp));
    analytics->update(create_snapshot(42100.0, 2.0, 42101.0, 1.0, timestamp + 1'000'000'000));

    // All metrics should be updated
    EXPECT_GT(analytics->get_vwap(), 0.0);
    EXPECT_GT(analytics->get_twap(), 0.0);

    // Second snapshot has more bid volume (imbalance > 0)
    EXPECT_GT(analytics->get_imbalance(), 0.0);
}

TEST_F(MarketAnalyticsTest, SymbolMatching) {
    EXPECT_EQ(analytics->get_symbol(), "BTCUSD");
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(VWAPTest, PerformanceUpdate) {
    const int num_updates = 100000;
    uint64_t timestamp = 1000000000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_updates; i++) {
        vwap->update(42000.0 + (i % 100), 1.0 + (i % 10), timestamp + i * 1000);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_updates;

    // Should be very fast (< 100 ns)
    EXPECT_LT(avg_ns, 100.0) << "Average VWAP update time: " << avg_ns << " ns";

    std::cout << "Average VWAP update time: " << avg_ns << " ns\n";
}

TEST_F(TWAPTest, PerformanceUpdate) {
    const int num_updates = 100000;
    uint64_t timestamp = 1000000000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_updates; i++) {
        twap->update(42000.0 + (i % 100), timestamp + i * 1000);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_updates;

    EXPECT_LT(avg_ns, 100.0) << "Average TWAP update time: " << avg_ns << " ns";

    std::cout << "Average TWAP update time: " << avg_ns << " ns\n";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
