/**
 * @file test_exchange_connectors.cpp
 * @brief Integration tests for exchange WebSocket connectors
 *
 * Tests real connections to exchange WebSocket APIs.
 * These tests connect to actual exchanges and verify data reception.
 *
 * **WARNING:** These tests connect to live APIs and may be rate-limited.
 * Run sparingly to avoid API bans.
 */

#include <gtest/gtest.h>
#include "exchange/binance_connector.hpp"
#include "exchange/coinbase_connector.hpp"
#include "exchange/kraken_connector.hpp"
#include "exchange/connection_manager.hpp"
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>

using namespace marketdata;

/**
 * @brief Test fixture for exchange connector tests
 */
class ExchangeConnectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set log level to debug for tests
        spdlog::set_level(spdlog::level::debug);
    }

    void TearDown() override {
        // Reset log level
        spdlog::set_level(spdlog::level::info);
    }
};

/**
 * @brief Test Binance connection and subscription
 *
 * Connects to Binance WebSocket and subscribes to BTC-USD.
 * Verifies that updates are received.
 */
TEST_F(ExchangeConnectorTest, BinanceConnection) {
    auto binance = std::make_unique<BinanceConnector>(false);

    // Track connection and data
    std::atomic<bool> connected{false};
    std::atomic<bool> data_received{false};
    std::atomic<int> update_count{0};

    // Set callbacks
    binance->set_connection_callback([&](bool is_connected, const std::string& msg) {
        if (is_connected) {
            connected.store(true);
            spdlog::info("Binance connected: {}", msg);
        }
    });

    binance->set_orderbook_callback([&](const NormalizedOrderBookUpdate& update) {
        data_received.store(true);
        update_count.fetch_add(1);
        spdlog::info("Binance update: {} ({} bids, {} asks)",
                    update.symbol, update.bids.size(), update.asks.size());
    });

    binance->set_error_callback([](const std::string& error) {
        spdlog::error("Binance error: {}", error);
    });

    // Connect
    ASSERT_TRUE(binance->connect());

    // Run I/O in background
    std::thread io_thread([&]() {
        binance->run();
    });

    // Wait for connection
    for (int i = 0; i < 50 && !connected.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(connected.load()) << "Failed to connect to Binance";

    // Subscribe to BTC-USDT
    ASSERT_TRUE(binance->subscribe("btcusdt"));

    // Wait for data (up to 10 seconds)
    for (int i = 0; i < 100 && !data_received.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Verify data received
    ASSERT_TRUE(data_received.load()) << "No data received from Binance";
    ASSERT_GT(update_count.load(), 0) << "Expected at least one update";

    spdlog::info("Binance test: Received {} updates", update_count.load());

    // Cleanup
    binance->stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    binance->disconnect();
}

/**
 * @brief Test Coinbase connection and subscription
 *
 * Connects to Coinbase WebSocket and subscribes to BTC-USD.
 * Verifies that snapshot and updates are received.
 */
TEST_F(ExchangeConnectorTest, CoinbaseConnection) {
    auto coinbase = std::make_unique<CoinbaseConnector>(false);

    std::atomic<bool> connected{false};
    std::atomic<bool> snapshot_received{false};
    std::atomic<bool> update_received{false};
    std::atomic<int> update_count{0};

    coinbase->set_connection_callback([&](bool is_connected, const std::string& msg) {
        if (is_connected) {
            connected.store(true);
            spdlog::info("Coinbase connected: {}", msg);
        }
    });

    coinbase->set_orderbook_callback([&](const NormalizedOrderBookUpdate& update) {
        update_count.fetch_add(1);

        // First update is usually the snapshot (large number of levels)
        if (!snapshot_received.load() && update.bids.size() > 10) {
            snapshot_received.store(true);
            spdlog::info("Coinbase snapshot: {} ({} bids, {} asks)",
                        update.symbol, update.bids.size(), update.asks.size());
        } else {
            update_received.store(true);
        }
    });

    coinbase->set_error_callback([](const std::string& error) {
        spdlog::error("Coinbase error: {}", error);
    });

    // Connect
    ASSERT_TRUE(coinbase->connect());

    std::thread io_thread([&]() {
        coinbase->run();
    });

    // Wait for connection
    for (int i = 0; i < 50 && !connected.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(connected.load()) << "Failed to connect to Coinbase";

    // Subscribe to BTC-USD
    ASSERT_TRUE(coinbase->subscribe("BTC-USD"));

    // Wait for snapshot and updates
    for (int i = 0; i < 100 && (!snapshot_received.load() || !update_received.load()); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ASSERT_TRUE(snapshot_received.load()) << "No snapshot received from Coinbase";
    ASSERT_TRUE(update_received.load()) << "No updates received from Coinbase";

    spdlog::info("Coinbase test: Received {} total updates", update_count.load());

    // Cleanup
    coinbase->stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    coinbase->disconnect();
}

/**
 * @brief Test Kraken connection and subscription
 *
 * Connects to Kraken WebSocket and subscribes to XBT/USD.
 * Verifies that snapshot and updates are received.
 */
TEST_F(ExchangeConnectorTest, KrakenConnection) {
    auto kraken = std::make_unique<KrakenConnector>(10);  // depth 10

    std::atomic<bool> connected{false};
    std::atomic<bool> data_received{false};
    std::atomic<int> update_count{0};

    kraken->set_connection_callback([&](bool is_connected, const std::string& msg) {
        if (is_connected) {
            connected.store(true);
            spdlog::info("Kraken connected: {}", msg);
        }
    });

    kraken->set_orderbook_callback([&](const NormalizedOrderBookUpdate& update) {
        data_received.store(true);
        update_count.fetch_add(1);
        spdlog::info("Kraken update: {} ({} bids, {} asks)",
                    update.symbol, update.bids.size(), update.asks.size());
    });

    kraken->set_error_callback([](const std::string& error) {
        spdlog::error("Kraken error: {}", error);
    });

    // Connect
    ASSERT_TRUE(kraken->connect());

    std::thread io_thread([&]() {
        kraken->run();
    });

    // Wait for connection
    for (int i = 0; i < 50 && !connected.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(connected.load()) << "Failed to connect to Kraken";

    // Subscribe to XBT/USD (Kraken uses XBT for Bitcoin)
    ASSERT_TRUE(kraken->subscribe("XBT/USD"));

    // Wait for data
    for (int i = 0; i < 100 && !data_received.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ASSERT_TRUE(data_received.load()) << "No data received from Kraken";
    ASSERT_GT(update_count.load(), 0) << "Expected at least one update";

    spdlog::info("Kraken test: Received {} updates", update_count.load());

    // Cleanup
    kraken->stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    kraken->disconnect();
}

/**
 * @brief Test connection manager with multiple exchanges
 *
 * Tests ConnectionManager coordinating multiple exchanges.
 */
TEST_F(ExchangeConnectorTest, ConnectionManager) {
    auto manager = std::make_unique<ConnectionManager>();

    std::atomic<int> connection_count{0};
    std::atomic<int> binance_updates{0};
    std::atomic<int> coinbase_updates{0};
    std::atomic<int> kraken_updates{0};

    // Set callbacks
    manager->set_connection_callback([&](Exchange ex, bool connected, const std::string& msg) {
        if (connected) {
            connection_count.fetch_add(1);
            spdlog::info("Exchange {} connected: {}", static_cast<int>(ex), msg);
        }
    });

    manager->set_orderbook_callback([&](const NormalizedOrderBookUpdate& update) {
        switch (update.exchange) {
            case Exchange::BINANCE:
                binance_updates.fetch_add(1);
                break;
            case Exchange::COINBASE:
                coinbase_updates.fetch_add(1);
                break;
            case Exchange::KRAKEN:
                kraken_updates.fetch_add(1);
                break;
        }
    });

    manager->set_error_callback([](Exchange ex, const std::string& error) {
        spdlog::error("Exchange {} error: {}", static_cast<int>(ex), error);
    });

    // Add exchanges
    ASSERT_TRUE(manager->add_exchange(Exchange::BINANCE));
    ASSERT_TRUE(manager->add_exchange(Exchange::COINBASE));
    ASSERT_TRUE(manager->add_exchange(Exchange::KRAKEN, 10));

    // Start all
    ASSERT_TRUE(manager->start_all());

    // Wait for connections (all 3)
    for (int i = 0; i < 100 && connection_count.load() < 3; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_EQ(connection_count.load(), 3) << "Not all exchanges connected";

    // Subscribe to symbols
    ASSERT_TRUE(manager->subscribe(Exchange::BINANCE, "btcusdt"));
    ASSERT_TRUE(manager->subscribe(Exchange::COINBASE, "BTC-USD"));
    ASSERT_TRUE(manager->subscribe(Exchange::KRAKEN, "XBT/USD"));

    // Wait for data from all exchanges
    for (int i = 0; i < 100; i++) {
        if (binance_updates.load() > 0 &&
            coinbase_updates.load() > 0 &&
            kraken_updates.load() > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Verify data received from all
    EXPECT_GT(binance_updates.load(), 0) << "No Binance updates";
    EXPECT_GT(coinbase_updates.load(), 0) << "No Coinbase updates";
    EXPECT_GT(kraken_updates.load(), 0) << "No Kraken updates";

    spdlog::info("ConnectionManager test: Binance={}, Coinbase={}, Kraken={}",
                binance_updates.load(), coinbase_updates.load(), kraken_updates.load());

    // Test connection state queries
    EXPECT_TRUE(manager->is_connected(Exchange::BINANCE));
    EXPECT_TRUE(manager->is_connected(Exchange::COINBASE));
    EXPECT_TRUE(manager->is_connected(Exchange::KRAKEN));
    EXPECT_EQ(manager->get_active_connections(), 3);

    // Cleanup
    manager->stop_all();
}

/**
 * @brief Test reconnection logic
 *
 * Tests that connectors can reconnect after disconnect.
 * This is a simplified test that manually disconnects and reconnects.
 */
TEST_F(ExchangeConnectorTest, Reconnection) {
    auto binance = std::make_unique<BinanceConnector>(false);

    std::atomic<int> connection_count{0};
    std::atomic<int> disconnection_count{0};

    binance->set_connection_callback([&](bool is_connected, const std::string& msg) {
        if (is_connected) {
            connection_count.fetch_add(1);
            spdlog::info("Binance connected (count: {})", connection_count.load());
        } else {
            disconnection_count.fetch_add(1);
            spdlog::info("Binance disconnected (count: {})", disconnection_count.load());
        }
    });

    binance->set_error_callback([](const std::string& error) {
        spdlog::error("Binance error: {}", error);
    });

    // Disable auto-reconnect for this test
    binance->set_auto_reconnect(false);

    // First connection
    ASSERT_TRUE(binance->connect());

    std::thread io_thread([&]() {
        binance->run();
    });

    // Wait for connection
    for (int i = 0; i < 50 && connection_count.load() < 1; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_EQ(connection_count.load(), 1) << "Failed initial connection";

    // Disconnect
    binance->stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    binance->disconnect();

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Reconnect
    ASSERT_TRUE(binance->connect());

    std::thread io_thread2([&]() {
        binance->run();
    });

    // Wait for reconnection
    for (int i = 0; i < 50 && connection_count.load() < 2; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_EQ(connection_count.load(), 2) << "Failed to reconnect";

    // Cleanup
    binance->stop();
    if (io_thread2.joinable()) {
        io_thread2.join();
    }
    binance->disconnect();
}

/**
 * @brief Test multiple subscriptions
 *
 * Tests subscribing to multiple symbols on same exchange.
 */
TEST_F(ExchangeConnectorTest, MultipleSubscriptions) {
    auto binance = std::make_unique<BinanceConnector>(false);

    std::atomic<bool> connected{false};
    std::set<std::string> symbols_received;
    std::mutex symbols_mutex;

    binance->set_connection_callback([&](bool is_connected, const std::string& msg) {
        if (is_connected) {
            connected.store(true);
        }
    });

    binance->set_orderbook_callback([&](const NormalizedOrderBookUpdate& update) {
        std::lock_guard<std::mutex> lock(symbols_mutex);
        symbols_received.insert(update.symbol);
    });

    // Connect
    ASSERT_TRUE(binance->connect());

    std::thread io_thread([&]() {
        binance->run();
    });

    // Wait for connection
    for (int i = 0; i < 50 && !connected.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(connected.load());

    // Subscribe to multiple symbols
    ASSERT_TRUE(binance->subscribe("btcusdt"));
    ASSERT_TRUE(binance->subscribe("ethusdt"));
    ASSERT_TRUE(binance->subscribe("bnbusdt"));

    // Wait for data from all symbols
    for (int i = 0; i < 200; i++) {
        std::lock_guard<std::mutex> lock(symbols_mutex);
        if (symbols_received.size() >= 3) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Verify received data from all symbols
    {
        std::lock_guard<std::mutex> lock(symbols_mutex);
        EXPECT_GE(symbols_received.size(), 2)
            << "Expected data from at least 2 symbols";

        for (const auto& symbol : symbols_received) {
            spdlog::info("Received updates for: {}", symbol);
        }
    }

    // Cleanup
    binance->stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    binance->disconnect();
}

/**
 * @brief Main function
 */
int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
