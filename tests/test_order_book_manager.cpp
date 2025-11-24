/**
 * @file test_order_book_manager.cpp
 * @brief Unit tests for OrderBookManager
 */

#include <gtest/gtest.h>
#include "core/order_book_manager.hpp"

using namespace marketdata;

class OrderBookManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager = std::make_unique<OrderBookManager>();
    }

    std::unique_ptr<OrderBookManager> manager;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(OrderBookManagerTest, InitialState) {
    EXPECT_EQ(manager->get_book_count(), 0);
}

TEST_F(OrderBookManagerTest, AddOrderBook) {
    bool added = manager->add_order_book("BTCUSD", Exchange::BINANCE);

    EXPECT_TRUE(added);
    EXPECT_EQ(manager->get_book_count(), 1);
}

TEST_F(OrderBookManagerTest, AddMultipleOrderBooks) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    manager->add_order_book("ETHUSD", Exchange::BINANCE);
    manager->add_order_book("BTCUSD", Exchange::COINBASE);

    EXPECT_EQ(manager->get_book_count(), 3);
}

TEST_F(OrderBookManagerTest, AddDuplicateOrderBook) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    bool added_again = manager->add_order_book("BTCUSD", Exchange::BINANCE);

    EXPECT_FALSE(added_again);  // Should reject duplicate
    EXPECT_EQ(manager->get_book_count(), 1);
}

TEST_F(OrderBookManagerTest, GetOrderBook) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);

    auto book = manager->get_order_book("BTCUSD", Exchange::BINANCE);

    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->get_symbol(), "BTCUSD");
    EXPECT_EQ(book->get_exchange(), Exchange::BINANCE);
}

TEST_F(OrderBookManagerTest, GetNonexistentOrderBook) {
    auto book = manager->get_order_book("BTCUSD", Exchange::BINANCE);

    EXPECT_EQ(book, nullptr);
}

// ============================================================================
// Order Book Lifecycle
// ============================================================================

TEST_F(OrderBookManagerTest, RemoveOrderBook) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    EXPECT_EQ(manager->get_book_count(), 1);

    bool removed = manager->remove_order_book("BTCUSD", Exchange::BINANCE);

    EXPECT_TRUE(removed);
    EXPECT_EQ(manager->get_book_count(), 0);
}

TEST_F(OrderBookManagerTest, RemoveNonexistentOrderBook) {
    bool removed = manager->remove_order_book("BTCUSD", Exchange::BINANCE);

    EXPECT_FALSE(removed);
}

TEST_F(OrderBookManagerTest, ClearAllOrderBooks) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    manager->add_order_book("ETHUSD", Exchange::BINANCE);
    manager->add_order_book("BTCUSD", Exchange::COINBASE);

    manager->clear();

    EXPECT_EQ(manager->get_book_count(), 0);
}

// ============================================================================
// Symbol Queries
// ============================================================================

TEST_F(OrderBookManagerTest, GetSymbols) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    manager->add_order_book("ETHUSD", Exchange::BINANCE);
    manager->add_order_book("BTCUSD", Exchange::COINBASE);

    auto symbols = manager->get_symbols();

    EXPECT_EQ(symbols.size(), 2);  // BTCUSD and ETHUSD
    EXPECT_TRUE(symbols.count("BTCUSD") > 0);
    EXPECT_TRUE(symbols.count("ETHUSD") > 0);
}

TEST_F(OrderBookManagerTest, GetExchangesForSymbol) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    manager->add_order_book("BTCUSD", Exchange::COINBASE);
    manager->add_order_book("BTCUSD", Exchange::KRAKEN);

    auto exchanges = manager->get_exchanges_for_symbol("BTCUSD");

    EXPECT_EQ(exchanges.size(), 3);
    EXPECT_TRUE(std::find(exchanges.begin(), exchanges.end(), Exchange::BINANCE) != exchanges.end());
    EXPECT_TRUE(std::find(exchanges.begin(), exchanges.end(), Exchange::COINBASE) != exchanges.end());
    EXPECT_TRUE(std::find(exchanges.begin(), exchanges.end(), Exchange::KRAKEN) != exchanges.end());
}

TEST_F(OrderBookManagerTest, GetExchangesForNonexistentSymbol) {
    auto exchanges = manager->get_exchanges_for_symbol("NONEXISTENT");

    EXPECT_TRUE(exchanges.empty());
}

TEST_F(OrderBookManagerTest, GetOrderBooksForSymbol) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    manager->add_order_book("BTCUSD", Exchange::COINBASE);
    manager->add_order_book("ETHUSD", Exchange::BINANCE);

    auto books = manager->get_order_books_for_symbol("BTCUSD");

    EXPECT_EQ(books.size(), 2);

    for (auto& book : books) {
        EXPECT_EQ(book->get_symbol(), "BTCUSD");
    }
}

// ============================================================================
// Update Operations
// ============================================================================

TEST_F(OrderBookManagerTest, UpdateOrderBook) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);

    auto book = manager->get_order_book("BTCUSD", Exchange::BINANCE);
    ASSERT_NE(book, nullptr);

    // Update the book
    bool updated = book->update_bid(42000.0, 1.5, 1);
    EXPECT_TRUE(updated);

    // Verify update
    auto snapshot = book->get_snapshot(10);
    ASSERT_EQ(snapshot.bids.size(), 1);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 42000.0);
}

TEST_F(OrderBookManagerTest, UpdateMultipleBooks) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    manager->add_order_book("BTCUSD", Exchange::COINBASE);

    auto binance = manager->get_order_book("BTCUSD", Exchange::BINANCE);
    auto coinbase = manager->get_order_book("BTCUSD", Exchange::COINBASE);

    binance->update_bid(42000.0, 1.0, 1);
    coinbase->update_bid(42001.0, 2.0, 1);

    auto binance_snapshot = binance->get_snapshot(10);
    auto coinbase_snapshot = coinbase->get_snapshot(10);

    EXPECT_DOUBLE_EQ(binance_snapshot.bids[0].price, 42000.0);
    EXPECT_DOUBLE_EQ(coinbase_snapshot.bids[0].price, 42001.0);
}

// ============================================================================
// Callback Tests
// ============================================================================

TEST_F(OrderBookManagerTest, UpdateCallback) {
    bool callback_invoked = false;
    std::string callback_symbol;
    Exchange callback_exchange = Exchange::BINANCE;

    manager->set_update_callback([&](const std::string& symbol, Exchange exchange, const OrderBookSnapshot&) {
        callback_invoked = true;
        callback_symbol = symbol;
        callback_exchange = exchange;
    });

    manager->add_order_book("BTCUSD", Exchange::BINANCE);

    auto book = manager->get_order_book("BTCUSD", Exchange::BINANCE);
    book->update_bid(42000.0, 1.0, 1);

    // Manually trigger callback (in real implementation, this would be automatic)
    auto snapshot = book->get_snapshot(10);
    if (manager->has_update_callback()) {
        manager->trigger_update_callback("BTCUSD", Exchange::BINANCE, snapshot);
    }

    EXPECT_TRUE(callback_invoked);
    EXPECT_EQ(callback_symbol, "BTCUSD");
    EXPECT_EQ(callback_exchange, Exchange::BINANCE);
}

// ============================================================================
// Statistics
// ============================================================================

TEST_F(OrderBookManagerTest, GetStatistics) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    manager->add_order_book("ETHUSD", Exchange::BINANCE);
    manager->add_order_book("BTCUSD", Exchange::COINBASE);

    auto stats = manager->get_statistics();

    EXPECT_EQ(stats.total_books, 3);
    EXPECT_EQ(stats.symbols.size(), 2);
    EXPECT_EQ(stats.exchanges.size(), 2);
}

TEST_F(OrderBookManagerTest, GetSymbolStatistics) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    manager->add_order_book("BTCUSD", Exchange::COINBASE);

    auto binance = manager->get_order_book("BTCUSD", Exchange::BINANCE);
    auto coinbase = manager->get_order_book("BTCUSD", Exchange::COINBASE);

    binance->update_bid(42000.0, 1.0, 1);
    binance->update_ask(42001.0, 1.0, 1);

    coinbase->update_bid(41999.0, 2.0, 1);
    coinbase->update_ask(42002.0, 2.0, 1);

    auto symbol_stats = manager->get_symbol_statistics("BTCUSD");

    EXPECT_EQ(symbol_stats.exchange_count, 2);
    EXPECT_GT(symbol_stats.best_bid, 0.0);
    EXPECT_GT(symbol_stats.best_ask, 0.0);
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST_F(OrderBookManagerTest, ConcurrentAdd) {
    const int num_threads = 8;
    const int books_per_thread = 10;

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, t, books_per_thread]() {
            for (int i = 0; i < books_per_thread; i++) {
                std::string symbol = "SYM" + std::to_string(t * books_per_thread + i);
                manager->add_order_book(symbol, Exchange::BINANCE);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(manager->get_book_count(), num_threads * books_per_thread);
}

TEST_F(OrderBookManagerTest, ConcurrentGetAndUpdate) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);

    const int num_threads = 4;
    const int updates_per_thread = 100;

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, t, updates_per_thread]() {
            auto book = manager->get_order_book("BTCUSD", Exchange::BINANCE);
            if (book) {
                for (int i = 0; i < updates_per_thread; i++) {
                    double price = 42000.0 + t * 100 + i;
                    book->update_bid(price, 1.0, 1);
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto book = manager->get_order_book("BTCUSD", Exchange::BINANCE);
    auto snapshot = book->get_snapshot(500);

    // Should have many levels
    EXPECT_GT(snapshot.bids.size(), 0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(OrderBookManagerTest, EmptySymbol) {
    bool added = manager->add_order_book("", Exchange::BINANCE);

    // Should reject empty symbol
    EXPECT_FALSE(added);
}

TEST_F(OrderBookManagerTest, VeryLongSymbol) {
    std::string long_symbol(1000, 'A');
    bool added = manager->add_order_book(long_symbol, Exchange::BINANCE);

    // Should still work (or reject gracefully)
    // Implementation dependent
}

TEST_F(OrderBookManagerTest, ManySymbols) {
    // Add many order books
    for (int i = 0; i < 100; i++) {
        std::string symbol = "SYM" + std::to_string(i);
        manager->add_order_book(symbol, Exchange::BINANCE);
    }

    EXPECT_EQ(manager->get_book_count(), 100);

    // Should still be able to retrieve
    auto book = manager->get_order_book("SYM50", Exchange::BINANCE);
    EXPECT_NE(book, nullptr);
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(OrderBookManagerTest, PerformanceAdd) {
    const int num_books = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_books; i++) {
        std::string symbol = "SYM" + std::to_string(i);
        manager->add_order_book(symbol, Exchange::BINANCE);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_books;

    std::cout << "Average add_order_book time: " << avg_ns << " ns\n";

    // Should be reasonably fast
    EXPECT_LT(avg_ns, 10000.0);  // < 10 microseconds
}

TEST_F(OrderBookManagerTest, PerformanceGet) {
    manager->add_order_book("BTCUSD", Exchange::BINANCE);

    const int num_gets = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_gets; i++) {
        auto book = manager->get_order_book("BTCUSD", Exchange::BINANCE);
        volatile auto ptr = book.get();  // Prevent optimization
        (void)ptr;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_gets;

    std::cout << "Average get_order_book time: " << avg_ns << " ns\n";

    // Should be very fast (hash map lookup)
    EXPECT_LT(avg_ns, 100.0);  // < 100 ns
}

// ============================================================================
// Real-World Scenarios
// ============================================================================

TEST_F(OrderBookManagerTest, MultiExchangeSetup) {
    // Typical multi-exchange setup
    std::vector<std::string> symbols = {"BTCUSD", "ETHUSD", "BNBUSD"};
    std::vector<Exchange> exchanges = {Exchange::BINANCE, Exchange::COINBASE, Exchange::KRAKEN};

    for (const auto& symbol : symbols) {
        for (const auto& exchange : exchanges) {
            manager->add_order_book(symbol, exchange);
        }
    }

    EXPECT_EQ(manager->get_book_count(), symbols.size() * exchanges.size());

    // Verify all combinations exist
    for (const auto& symbol : symbols) {
        auto books = manager->get_order_books_for_symbol(symbol);
        EXPECT_EQ(books.size(), exchanges.size());
    }
}

TEST_F(OrderBookManagerTest, DynamicAddRemove) {
    // Add books
    manager->add_order_book("BTCUSD", Exchange::BINANCE);
    manager->add_order_book("ETHUSD", Exchange::BINANCE);

    // Remove one
    manager->remove_order_book("BTCUSD", Exchange::BINANCE);

    // Add another
    manager->add_order_book("BNBUSD", Exchange::BINANCE);

    auto symbols = manager->get_symbols();
    EXPECT_EQ(symbols.size(), 2);  // ETHUSD and BNBUSD
    EXPECT_TRUE(symbols.count("ETHUSD") > 0);
    EXPECT_TRUE(symbols.count("BNBUSD") > 0);
    EXPECT_FALSE(symbols.count("BTCUSD") > 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
