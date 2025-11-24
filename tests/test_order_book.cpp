/**
 * @file test_order_book.cpp
 * @brief Unit tests for OrderBook class
 */

#include <gtest/gtest.h>
#include "core/order_book.hpp"
#include <thread>
#include <vector>

using namespace marketdata;

class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        order_book = std::make_unique<OrderBook<1000>>("BTCUSD", Exchange::BINANCE);
    }

    std::unique_ptr<OrderBook<1000>> order_book;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(OrderBookTest, InitialState) {
    auto snapshot = order_book->get_snapshot(10);

    EXPECT_EQ(snapshot.symbol, "BTCUSD");
    EXPECT_EQ(snapshot.exchange, Exchange::BINANCE);
    EXPECT_TRUE(snapshot.bids.empty());
    EXPECT_TRUE(snapshot.asks.empty());
    EXPECT_FALSE(snapshot.get_best_bid().has_value());
    EXPECT_FALSE(snapshot.get_best_ask().has_value());
    EXPECT_FALSE(snapshot.get_mid_price().has_value());
    EXPECT_FALSE(snapshot.get_spread().has_value());
}

TEST_F(OrderBookTest, AddSingleBid) {
    EXPECT_TRUE(order_book->update_bid(42000.0, 1.5, 1));

    auto snapshot = order_book->get_snapshot(10);

    ASSERT_EQ(snapshot.bids.size(), 1);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 42000.0);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].quantity, 1.5);
    EXPECT_EQ(snapshot.bids[0].order_count, 1);

    auto best_bid = snapshot.get_best_bid();
    ASSERT_TRUE(best_bid.has_value());
    EXPECT_DOUBLE_EQ(best_bid->price, 42000.0);
}

TEST_F(OrderBookTest, AddSingleAsk) {
    EXPECT_TRUE(order_book->update_ask(42001.0, 2.0, 1));

    auto snapshot = order_book->get_snapshot(10);

    ASSERT_EQ(snapshot.asks.size(), 1);
    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 42001.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[0].quantity, 2.0);
    EXPECT_EQ(snapshot.asks[0].order_count, 1);

    auto best_ask = snapshot.get_best_ask();
    ASSERT_TRUE(best_ask.has_value());
    EXPECT_DOUBLE_EQ(best_ask->price, 42001.0);
}

TEST_F(OrderBookTest, RemoveBid) {
    order_book->update_bid(42000.0, 1.5, 1);
    order_book->update_bid(42000.0, 0.0, 0);  // Remove

    auto snapshot = order_book->get_snapshot(10);
    EXPECT_TRUE(snapshot.bids.empty());
}

TEST_F(OrderBookTest, RemoveAsk) {
    order_book->update_ask(42001.0, 2.0, 1);
    order_book->update_ask(42001.0, 0.0, 0);  // Remove

    auto snapshot = order_book->get_snapshot(10);
    EXPECT_TRUE(snapshot.asks.empty());
}

// ============================================================================
// Ordering
// ============================================================================

TEST_F(OrderBookTest, BidsDescendingOrder) {
    // Add bids in random order
    order_book->update_bid(42000.0, 1.0, 1);
    order_book->update_bid(42002.0, 1.5, 1);  // Higher price
    order_book->update_bid(41998.0, 2.0, 1);  // Lower price
    order_book->update_bid(42001.0, 1.2, 1);

    auto snapshot = order_book->get_snapshot(10);

    ASSERT_EQ(snapshot.bids.size(), 4);

    // Should be sorted in descending order (best bid first)
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 42002.0);
    EXPECT_DOUBLE_EQ(snapshot.bids[1].price, 42001.0);
    EXPECT_DOUBLE_EQ(snapshot.bids[2].price, 42000.0);
    EXPECT_DOUBLE_EQ(snapshot.bids[3].price, 41998.0);
}

TEST_F(OrderBookTest, AsksAscendingOrder) {
    // Add asks in random order
    order_book->update_ask(42001.0, 1.0, 1);
    order_book->update_ask(41999.0, 1.5, 1);  // Lower price
    order_book->update_ask(42003.0, 2.0, 1);  // Higher price
    order_book->update_ask(42000.0, 1.2, 1);

    auto snapshot = order_book->get_snapshot(10);

    ASSERT_EQ(snapshot.asks.size(), 4);

    // Should be sorted in ascending order (best ask first)
    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 41999.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[1].price, 42000.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[2].price, 42001.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[3].price, 42003.0);
}

// ============================================================================
// Spread Calculation
// ============================================================================

TEST_F(OrderBookTest, SpreadCalculation) {
    order_book->update_bid(42000.0, 1.0, 1);
    order_book->update_ask(42001.0, 1.0, 1);

    auto snapshot = order_book->get_snapshot(10);

    auto spread = snapshot.get_spread();
    ASSERT_TRUE(spread.has_value());
    EXPECT_DOUBLE_EQ(*spread, 1.0);

    auto spread_bps = snapshot.get_spread_bps();
    ASSERT_TRUE(spread_bps.has_value());
    EXPECT_NEAR(*spread_bps, 2.38, 0.01);  // (1.0 / 42000.5) * 10000
}

TEST_F(OrderBookTest, MidPriceCalculation) {
    order_book->update_bid(42000.0, 1.0, 1);
    order_book->update_ask(42002.0, 1.0, 1);

    auto snapshot = order_book->get_snapshot(10);

    auto mid = snapshot.get_mid_price();
    ASSERT_TRUE(mid.has_value());
    EXPECT_DOUBLE_EQ(*mid, 42001.0);
}

// ============================================================================
// Depth Limiting
// ============================================================================

TEST_F(OrderBookTest, DepthLimiting) {
    // Add 20 bids
    for (int i = 0; i < 20; i++) {
        order_book->update_bid(42000.0 - i, 1.0, 1);
    }

    // Request only 5 levels
    auto snapshot = order_book->get_snapshot(5);

    ASSERT_EQ(snapshot.bids.size(), 5);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 42000.0);  // Best
    EXPECT_DOUBLE_EQ(snapshot.bids[4].price, 41996.0);  // 5th level
}

// ============================================================================
// Update Operations
// ============================================================================

TEST_F(OrderBookTest, UpdateExistingLevel) {
    order_book->update_bid(42000.0, 1.0, 1);
    order_book->update_bid(42000.0, 2.5, 2);  // Update quantity

    auto snapshot = order_book->get_snapshot(10);

    ASSERT_EQ(snapshot.bids.size(), 1);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].quantity, 2.5);
    EXPECT_EQ(snapshot.bids[0].order_count, 2);
}

TEST_F(OrderBookTest, MultipleUpdates) {
    // Initial state
    order_book->update_bid(42000.0, 1.0, 1);
    order_book->update_ask(42001.0, 1.0, 1);

    // Update quantities
    order_book->update_bid(42000.0, 2.0, 2);
    order_book->update_ask(42001.0, 3.0, 3);

    auto snapshot = order_book->get_snapshot(10);

    EXPECT_DOUBLE_EQ(snapshot.bids[0].quantity, 2.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[0].quantity, 3.0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(OrderBookTest, ZeroQuantityRemovesLevel) {
    order_book->update_bid(42000.0, 1.0, 1);
    order_book->update_bid(42000.0, 0.0, 0);

    auto snapshot = order_book->get_snapshot(10);
    EXPECT_TRUE(snapshot.bids.empty());
}

TEST_F(OrderBookTest, NegativePriceRejected) {
    EXPECT_FALSE(order_book->update_bid(-42000.0, 1.0, 1));
    EXPECT_FALSE(order_book->update_ask(-42001.0, 1.0, 1));
}

TEST_F(OrderBookTest, NegativeQuantityRejected) {
    EXPECT_FALSE(order_book->update_bid(42000.0, -1.0, 1));
    EXPECT_FALSE(order_book->update_ask(42001.0, -1.0, 1));
}

TEST_F(OrderBookTest, ZeroPriceRejected) {
    EXPECT_FALSE(order_book->update_bid(0.0, 1.0, 1));
    EXPECT_FALSE(order_book->update_ask(0.0, 1.0, 1));
}

// ============================================================================
// Snapshot Consistency
// ============================================================================

TEST_F(OrderBookTest, SnapshotConsistency) {
    // Add some levels
    order_book->update_bid(42000.0, 1.0, 1);
    order_book->update_bid(41999.0, 1.5, 2);
    order_book->update_ask(42001.0, 2.0, 1);
    order_book->update_ask(42002.0, 2.5, 2);

    auto snapshot1 = order_book->get_snapshot(10);
    auto snapshot2 = order_book->get_snapshot(10);

    // Both snapshots should be identical (SeqLock ensures consistency)
    EXPECT_EQ(snapshot1.bids.size(), snapshot2.bids.size());
    EXPECT_EQ(snapshot1.asks.size(), snapshot2.asks.size());

    for (size_t i = 0; i < snapshot1.bids.size(); i++) {
        EXPECT_DOUBLE_EQ(snapshot1.bids[i].price, snapshot2.bids[i].price);
        EXPECT_DOUBLE_EQ(snapshot1.bids[i].quantity, snapshot2.bids[i].quantity);
    }
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(OrderBookTest, PerformanceUpdate) {
    const int num_updates = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_updates; i++) {
        order_book->update_bid(42000.0 + (i % 100), 1.0 + (i % 10), 1);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_updates;

    // Should be sub-microsecond (< 1000 ns)
    EXPECT_LT(avg_ns, 1000.0) << "Average update time: " << avg_ns << " ns";

    std::cout << "Average update time: " << avg_ns << " ns\n";
}

TEST_F(OrderBookTest, PerformanceSnapshot) {
    // Add many levels
    for (int i = 0; i < 100; i++) {
        order_book->update_bid(42000.0 - i, 1.0, 1);
        order_book->update_ask(42001.0 + i, 1.0, 1);
    }

    const int num_snapshots = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_snapshots; i++) {
        auto snapshot = order_book->get_snapshot(20);
        // Prevent optimization
        volatile double mid = snapshot.get_mid_price().value_or(0.0);
        (void)mid;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_snapshots;

    // Should be sub-microsecond
    EXPECT_LT(avg_ns, 1000.0) << "Average snapshot time: " << avg_ns << " ns";

    std::cout << "Average snapshot time: " << avg_ns << " ns\n";
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST_F(OrderBookTest, ConcurrentUpdates) {
    const int num_threads = 4;
    const int updates_per_thread = 1000;

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, t, updates_per_thread]() {
            for (int i = 0; i < updates_per_thread; i++) {
                double price = 42000.0 + (t * 100) + i;
                order_book->update_bid(price, 1.0, 1);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto snapshot = order_book->get_snapshot(1000);

    // Should have many levels (exact count depends on overlaps)
    EXPECT_GT(snapshot.bids.size(), 0);
}

TEST_F(OrderBookTest, ConcurrentReaders) {
    // Add some levels
    for (int i = 0; i < 100; i++) {
        order_book->update_bid(42000.0 - i, 1.0, 1);
        order_book->update_ask(42001.0 + i, 1.0, 1);
    }

    const int num_readers = 8;
    const int reads_per_thread = 10000;

    std::vector<std::thread> threads;
    std::atomic<int> successful_reads{0};

    for (int t = 0; t < num_readers; t++) {
        threads.emplace_back([this, reads_per_thread, &successful_reads]() {
            for (int i = 0; i < reads_per_thread; i++) {
                auto snapshot = order_book->get_snapshot(20);
                if (!snapshot.bids.empty() && !snapshot.asks.empty()) {
                    successful_reads++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All reads should succeed
    EXPECT_EQ(successful_reads.load(), num_readers * reads_per_thread);
}

// ============================================================================
// Clear Operation
// ============================================================================

TEST_F(OrderBookTest, ClearOrderBook) {
    // Add levels
    order_book->update_bid(42000.0, 1.0, 1);
    order_book->update_bid(41999.0, 1.5, 2);
    order_book->update_ask(42001.0, 2.0, 1);
    order_book->update_ask(42002.0, 2.5, 2);

    order_book->clear();

    auto snapshot = order_book->get_snapshot(10);
    EXPECT_TRUE(snapshot.bids.empty());
    EXPECT_TRUE(snapshot.asks.empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
