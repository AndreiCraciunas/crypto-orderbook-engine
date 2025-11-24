/**
 * @file test_arbitrage_monitor.cpp
 * @brief Unit tests for ArbitrageMonitor
 */

#include <gtest/gtest.h>
#include "trading/arbitrage_monitor.hpp"
#include "core/aggregated_order_book.hpp"

using namespace marketdata;

class ArbitrageMonitorTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor = std::make_unique<ArbitrageMonitor>(0.1, 1000);
        orderbook = std::make_shared<AggregatedOrderBook>("BTCUSD");
    }

    NormalizedOrderBookUpdate create_update(Exchange exchange, double bid, double ask) {
        NormalizedOrderBookUpdate update;
        update.symbol = "BTCUSD";
        update.exchange = exchange;
        update.timestamp_ns = current_timestamp;
        update.is_snapshot = false;

        if (bid > 0) {
            OrderBookLevel bid_level;
            bid_level.price = bid;
            bid_level.quantity = 1.0;
            bid_level.order_count = 1;
            update.bids.push_back(bid_level);
        }

        if (ask > 0) {
            OrderBookLevel ask_level;
            ask_level.price = ask;
            ask_level.quantity = 1.0;
            ask_level.order_count = 1;
            update.asks.push_back(ask_level);
        }

        current_timestamp += 1000000;  // +1ms
        return update;
    }

    std::unique_ptr<ArbitrageMonitor> monitor;
    std::shared_ptr<AggregatedOrderBook> orderbook;
    uint64_t current_timestamp = 1000000000;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(ArbitrageMonitorTest, InitialState) {
    auto stats = monitor->get_statistics();

    EXPECT_EQ(stats.total_opportunities, 0);
    EXPECT_EQ(stats.active_opportunities, 0);
    EXPECT_DOUBLE_EQ(stats.best_profit_percentage, 0.0);
}

TEST_F(ArbitrageMonitorTest, AddOrderBook) {
    monitor->add_order_book(orderbook);

    // Should accept the order book
    auto stats = monitor->get_statistics();
    EXPECT_EQ(stats.total_opportunities, 0);
}

// ============================================================================
// Arbitrage Detection
// ============================================================================

TEST_F(ArbitrageMonitorTest, DetectSimpleArbitrage) {
    monitor->add_order_book(orderbook);

    bool alert_triggered = false;
    ArbitrageOpportunity captured_opp;

    monitor->set_alert_callback([&](const ArbitrageOpportunity& opp) {
        alert_triggered = true;
        captured_opp = opp;
    });

    // Binance: bid 42000, ask 42100
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 42100.0);
    orderbook->update(update1);
    monitor->check_opportunities();

    // Coinbase: bid 42200, ask 42300 (arbitrage possible)
    auto update2 = create_update(Exchange::COINBASE, 42200.0, 42300.0);
    orderbook->update(update2);
    monitor->check_opportunities();

    EXPECT_TRUE(alert_triggered);
    EXPECT_EQ(captured_opp.symbol, "BTCUSD");
    EXPECT_EQ(captured_opp.buy_exchange, Exchange::BINANCE);
    EXPECT_EQ(captured_opp.sell_exchange, Exchange::COINBASE);
    EXPECT_DOUBLE_EQ(captured_opp.buy_price, 42100.0);
    EXPECT_DOUBLE_EQ(captured_opp.sell_price, 42200.0);
    EXPECT_GT(captured_opp.profit_percentage, 0.1);

    auto stats = monitor->get_statistics();
    EXPECT_GT(stats.total_opportunities, 0);
}

TEST_F(ArbitrageMonitorTest, NoArbitrageWhenSpreadTight) {
    monitor->add_order_book(orderbook);

    bool alert_triggered = false;
    monitor->set_alert_callback([&](const ArbitrageOpportunity&) {
        alert_triggered = true;
    });

    // Binance: bid 42000, ask 42001
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 42001.0);
    orderbook->update(update1);
    monitor->check_opportunities();

    // Coinbase: bid 41999, ask 42002 (no arbitrage)
    auto update2 = create_update(Exchange::COINBASE, 41999.0, 42002.0);
    orderbook->update(update2);
    monitor->check_opportunities();

    EXPECT_FALSE(alert_triggered);
}

TEST_F(ArbitrageMonitorTest, ProfitThreshold) {
    // Higher threshold monitor
    auto strict_monitor = std::make_unique<ArbitrageMonitor>(1.0, 1000);  // 1% minimum
    strict_monitor->add_order_book(orderbook);

    bool alert_triggered = false;
    strict_monitor->set_alert_callback([&](const ArbitrageOpportunity&) {
        alert_triggered = true;
    });

    // Small arbitrage (< 1%)
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 42100.0);
    auto update2 = create_update(Exchange::COINBASE, 42120.0, 42200.0);

    orderbook->update(update1);
    orderbook->update(update2);
    strict_monitor->check_opportunities();

    // Should not trigger (profit too small)
    EXPECT_FALSE(alert_triggered);
}

// ============================================================================
// Opportunity Tracking
// ============================================================================

TEST_F(ArbitrageMonitorTest, ActiveOpportunities) {
    monitor->add_order_book(orderbook);

    // Create arbitrage
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 42100.0);
    auto update2 = create_update(Exchange::COINBASE, 42200.0, 42300.0);

    orderbook->update(update1);
    orderbook->update(update2);
    monitor->check_opportunities();

    auto active = monitor->get_active_opportunities();
    EXPECT_GT(active.size(), 0);

    // Check structure
    if (!active.empty()) {
        const auto& tracked = active[0];
        EXPECT_EQ(tracked.opportunity.symbol, "BTCUSD");
        EXPECT_GT(tracked.first_seen_ns, 0);
        EXPECT_GT(tracked.last_seen_ns, 0);
        EXPECT_GT(tracked.lifetime_ns, 0);
        EXPECT_GT(tracked.num_observations, 0);
        EXPECT_GT(tracked.max_profit, 0.0);
    }
}

TEST_F(ArbitrageMonitorTest, OpportunityExpiration) {
    monitor->add_order_book(orderbook);

    // Create arbitrage
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 42100.0);
    auto update2 = create_update(Exchange::COINBASE, 42200.0, 42300.0);

    orderbook->update(update1);
    orderbook->update(update2);
    monitor->check_opportunities();

    EXPECT_GT(monitor->get_active_opportunities().size(), 0);

    // Close arbitrage
    auto update3 = create_update(Exchange::COINBASE, 42000.0, 42050.0);
    orderbook->update(update3);
    monitor->check_opportunities();

    // Opportunity should expire (may take a check or two)
    for (int i = 0; i < 5; i++) {
        monitor->check_opportunities();
    }

    // Should eventually be removed from active list
    auto stats = monitor->get_statistics();
    EXPECT_EQ(stats.active_opportunities, 0);
}

// ============================================================================
// Statistics
// ============================================================================

TEST_F(ArbitrageMonitorTest, StatisticsAccumulation) {
    monitor->add_order_book(orderbook);

    // Create multiple arbitrages
    for (int i = 0; i < 5; i++) {
        auto update1 = create_update(Exchange::BINANCE, 42000.0, 42100.0);
        auto update2 = create_update(Exchange::COINBASE, 42200.0 + i * 10, 42300.0 + i * 10);

        orderbook->update(update1);
        orderbook->update(update2);
        monitor->check_opportunities();
    }

    auto stats = monitor->get_statistics();

    EXPECT_GT(stats.total_opportunities, 0);
    EXPECT_GT(stats.best_profit_percentage, 0.0);
    EXPECT_GT(stats.avg_profit_percentage, 0.0);
    EXPECT_GT(stats.avg_lifetime_ns, 0);
}

TEST_F(ArbitrageMonitorTest, BestProfitTracking) {
    monitor->add_order_book(orderbook);

    // Small arbitrage
    auto update1a = create_update(Exchange::BINANCE, 42000.0, 42100.0);
    auto update1b = create_update(Exchange::COINBASE, 42110.0, 42200.0);
    orderbook->update(update1a);
    orderbook->update(update1b);
    monitor->check_opportunities();

    auto stats1 = monitor->get_statistics();
    double first_profit = stats1.best_profit_percentage;

    // Larger arbitrage
    auto update2a = create_update(Exchange::BINANCE, 42000.0, 42100.0);
    auto update2b = create_update(Exchange::COINBASE, 42500.0, 42600.0);
    orderbook->update(update2a);
    orderbook->update(update2b);
    monitor->check_opportunities();

    auto stats2 = monitor->get_statistics();

    // Best profit should increase
    EXPECT_GT(stats2.best_profit_percentage, first_profit);
}

// ============================================================================
// Multiple Order Books
// ============================================================================

TEST_F(ArbitrageMonitorTest, MultipleSymbols) {
    auto btc_book = std::make_shared<AggregatedOrderBook>("BTCUSD");
    auto eth_book = std::make_shared<AggregatedOrderBook>("ETHUSD");

    monitor->add_order_book(btc_book);
    monitor->add_order_book(eth_book);

    int btc_alerts = 0;
    int eth_alerts = 0;

    monitor->set_alert_callback([&](const ArbitrageOpportunity& opp) {
        if (opp.symbol == "BTCUSD") {
            btc_alerts++;
        } else if (opp.symbol == "ETHUSD") {
            eth_alerts++;
        }
    });

    // BTC arbitrage
    NormalizedOrderBookUpdate btc1;
    btc1.symbol = "BTCUSD";
    btc1.exchange = Exchange::BINANCE;
    btc1.timestamp_ns = current_timestamp++;
    OrderBookLevel btc_ask;
    btc_ask.price = 42100.0;
    btc_ask.quantity = 1.0;
    btc1.asks.push_back(btc_ask);

    NormalizedOrderBookUpdate btc2;
    btc2.symbol = "BTCUSD";
    btc2.exchange = Exchange::COINBASE;
    btc2.timestamp_ns = current_timestamp++;
    OrderBookLevel btc_bid;
    btc_bid.price = 42200.0;
    btc_bid.quantity = 1.0;
    btc2.bids.push_back(btc_bid);

    btc_book->update(btc1);
    btc_book->update(btc2);
    monitor->check_opportunities();

    // ETH arbitrage
    NormalizedOrderBookUpdate eth1;
    eth1.symbol = "ETHUSD";
    eth1.exchange = Exchange::BINANCE;
    eth1.timestamp_ns = current_timestamp++;
    OrderBookLevel eth_ask;
    eth_ask.price = 2200.0;
    eth_ask.quantity = 10.0;
    eth1.asks.push_back(eth_ask);

    NormalizedOrderBookUpdate eth2;
    eth2.symbol = "ETHUSD";
    eth2.exchange = Exchange::COINBASE;
    eth2.timestamp_ns = current_timestamp++;
    OrderBookLevel eth_bid;
    eth_bid.price = 2210.0;
    eth_bid.quantity = 10.0;
    eth2.bids.push_back(eth_bid);

    eth_book->update(eth1);
    eth_book->update(eth2);
    monitor->check_opportunities();

    // Should detect both
    EXPECT_GT(btc_alerts, 0);
    EXPECT_GT(eth_alerts, 0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(ArbitrageMonitorTest, NoOrderBooksRegistered) {
    // Should not crash
    EXPECT_NO_THROW({
        monitor->check_opportunities();
    });

    auto stats = monitor->get_statistics();
    EXPECT_EQ(stats.total_opportunities, 0);
}

TEST_F(ArbitrageMonitorTest, EmptyOrderBook) {
    monitor->add_order_book(orderbook);

    // Check with empty book
    EXPECT_NO_THROW({
        monitor->check_opportunities();
    });

    auto stats = monitor->get_statistics();
    EXPECT_EQ(stats.total_opportunities, 0);
}

TEST_F(ArbitrageMonitorTest, RapidUpdates) {
    monitor->add_order_book(orderbook);

    // Rapidly changing prices
    for (int i = 0; i < 100; i++) {
        auto update1 = create_update(Exchange::BINANCE, 42000.0 + i, 42100.0 + i);
        auto update2 = create_update(Exchange::COINBASE, 42000.0 + i + 50, 42100.0 + i + 50);

        orderbook->update(update1);
        orderbook->update(update2);
        monitor->check_opportunities();
    }

    // Should handle without issues
    auto stats = monitor->get_statistics();
    EXPECT_GE(stats.total_opportunities, 0);
}

// ============================================================================
// Performance
// ============================================================================

TEST_F(ArbitrageMonitorTest, PerformanceCheck) {
    monitor->add_order_book(orderbook);

    // Setup orderbook with arbitrage
    auto update1 = create_update(Exchange::BINANCE, 42000.0, 42100.0);
    auto update2 = create_update(Exchange::COINBASE, 42200.0, 42300.0);
    orderbook->update(update1);
    orderbook->update(update2);

    const int num_checks = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_checks; i++) {
        monitor->check_opportunities();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_checks;

    // Should be very fast (< 1000 ns)
    EXPECT_LT(avg_ns, 1000.0) << "Average check time: " << avg_ns << " ns";

    std::cout << "Arbitrage check avg: " << avg_ns << " ns\n";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
