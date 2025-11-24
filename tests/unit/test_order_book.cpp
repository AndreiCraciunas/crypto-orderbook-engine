#include <gtest/gtest.h>
#include "core/order_book.hpp"
#include "utils/time_utils.hpp"
#include <thread>
#include <random>
#include <vector>

using namespace marketdata;

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook<100> book;

    void SetUp() override {
        // Initialize with some data
        book.update_bid(100.0, 10.0);
        book.update_bid(99.5, 20.0);
        book.update_ask(100.5, 15.0);
        book.update_ask(101.0, 25.0);
    }
};

TEST_F(OrderBookTest, UpdateBid) {
    book.update_bid(99.0, 30.0);

    auto snapshot = book.get_snapshot(5);
    ASSERT_EQ(snapshot.bids.size(), 3);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 100.0);
    EXPECT_DOUBLE_EQ(snapshot.bids[1].price, 99.5);
    EXPECT_DOUBLE_EQ(snapshot.bids[2].price, 99.0);
}

TEST_F(OrderBookTest, UpdateAsk) {
    book.update_ask(102.0, 35.0);

    auto snapshot = book.get_snapshot(5);
    ASSERT_EQ(snapshot.asks.size(), 3);
    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 100.5);
    EXPECT_DOUBLE_EQ(snapshot.asks[1].price, 101.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[2].price, 102.0);
}

TEST_F(OrderBookTest, RemoveLevel) {
    book.update_bid(100.0, 0.0);  // Remove level

    auto snapshot = book.get_snapshot(5);
    ASSERT_EQ(snapshot.bids.size(), 1);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 99.5);
}

TEST_F(OrderBookTest, GetSpread) {
    double spread = book.get_spread();
    EXPECT_NEAR(spread, 0.5, 0.00001);  // 100.5 - 100.0
}

TEST_F(OrderBookTest, GetMidPrice) {
    double mid = book.get_mid_price();
    EXPECT_NEAR(mid, 100.25, 0.00001);  // (100.0 + 100.5) / 2
}

TEST_F(OrderBookTest, GetBookImbalance) {
    double imbalance = book.get_book_imbalance();
    // (10 + 20) - (15 + 25) = -10
    // -10 / 70 = -0.142857...
    EXPECT_NEAR(imbalance, -0.142857, 0.000001);
}

TEST_F(OrderBookTest, ClearBook) {
    book.clear();

    auto snapshot = book.get_snapshot(5);
    EXPECT_EQ(snapshot.bids.size(), 0);
    EXPECT_EQ(snapshot.asks.size(), 0);
}

TEST_F(OrderBookTest, GetStatistics) {
    auto stats = book.get_statistics();

    EXPECT_NEAR(stats.spread, 0.5, 0.00001);
    EXPECT_NEAR(stats.mid_price, 100.25, 0.00001);
    EXPECT_EQ(stats.bid_depth, 2);
    EXPECT_EQ(stats.ask_depth, 2);
}

TEST_F(OrderBookTest, ConcurrentUpdates) {
    const int num_threads = 10;
    const int updates_per_thread = 10000;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([this, i, updates_per_thread]() {
            std::mt19937 gen(i);
            std::uniform_real_distribution<> price_dist(90.0, 110.0);
            std::uniform_real_distribution<> qty_dist(0.1, 100.0);

            for (int j = 0; j < updates_per_thread; ++j) {
                double price = price_dist(gen);
                double qty = qty_dist(gen);

                if (price < 100.0) {
                    book.update_bid(price, qty);
                } else {
                    book.update_ask(price, qty);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Verify consistency
    auto snapshot = book.get_snapshot(100);

    // Bids should be sorted descending
    for (size_t i = 1; i < snapshot.bids.size(); ++i) {
        EXPECT_GE(snapshot.bids[i-1].price, snapshot.bids[i].price);
    }

    // Asks should be sorted ascending
    for (size_t i = 1; i < snapshot.asks.size(); ++i) {
        EXPECT_LE(snapshot.asks[i-1].price, snapshot.asks[i].price);
    }

    // Best bid should be less than best ask
    if (!snapshot.bids.empty() && !snapshot.asks.empty()) {
        EXPECT_LT(snapshot.bids[0].price, snapshot.asks[0].price);
    }
}

TEST_F(OrderBookTest, SnapshotDepth) {
    // Add more levels
    for (int i = 0; i < 50; ++i) {
        book.update_bid(99.0 - i * 0.1, 10.0 + i);
        book.update_ask(101.0 + i * 0.1, 10.0 + i);
    }

    // Request depth of 10
    auto snapshot = book.get_snapshot(10);

    EXPECT_LE(snapshot.bids.size(), 10);
    EXPECT_LE(snapshot.asks.size(), 10);

    // Verify best prices are at front
    if (!snapshot.bids.empty()) {
        EXPECT_NEAR(snapshot.bids[0].price, 100.0, 0.00001);
    }

    if (!snapshot.asks.empty()) {
        EXPECT_NEAR(snapshot.asks[0].price, 100.5, 0.00001);
    }
}

TEST_F(OrderBookTest, UpdateExistingLevel) {
    // Update existing bid level
    book.update_bid(100.0, 50.0);

    auto snapshot = book.get_snapshot(5);
    ASSERT_EQ(snapshot.bids.size(), 2);

    // Find the updated level
    auto it = std::find_if(snapshot.bids.begin(), snapshot.bids.end(),
                           [](const PriceLevelSnapshot& level) {
                               return std::abs(level.price - 100.0) < 0.00001;
                           });

    ASSERT_NE(it, snapshot.bids.end());
    EXPECT_NEAR(it->quantity, 50.0, 0.00001);
}

// Test fixed-point conversion
TEST(TypesTest, FixedPointConversion) {
    double original = 123.456789;
    uint64_t fixed = double_to_fixed(original);
    double converted = fixed_to_double(fixed);

    EXPECT_NEAR(original, converted, 0.00000001);
}

// Test PriceIndexMap
TEST(PriceIndexMapTest, InsertAndFind) {
    PriceIndexMap map;

    uint64_t price1 = double_to_fixed(100.0);
    uint64_t price2 = double_to_fixed(99.5);

    EXPECT_TRUE(map.insert(price1, 0));
    EXPECT_TRUE(map.insert(price2, 1));

    auto idx1 = map.find(price1);
    auto idx2 = map.find(price2);

    ASSERT_TRUE(idx1.has_value());
    ASSERT_TRUE(idx2.has_value());
    EXPECT_EQ(*idx1, 0);
    EXPECT_EQ(*idx2, 1);
}

TEST(PriceIndexMapTest, Remove) {
    PriceIndexMap map;

    uint64_t price = double_to_fixed(100.0);
    map.insert(price, 0);

    EXPECT_TRUE(map.remove(price));
    EXPECT_FALSE(map.find(price).has_value());
}

TEST(PriceIndexMapTest, Clear) {
    PriceIndexMap map;

    map.insert(double_to_fixed(100.0), 0);
    map.insert(double_to_fixed(99.5), 1);

    map.clear();

    EXPECT_FALSE(map.find(double_to_fixed(100.0)).has_value());
    EXPECT_FALSE(map.find(double_to_fixed(99.5)).has_value());
}
