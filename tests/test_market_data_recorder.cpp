/**
 * @file test_market_data_recorder.cpp
 * @brief Unit tests for MarketDataRecorder and MarketDataReplayer
 */

#include <gtest/gtest.h>
#include "persistence/market_data_recorder.hpp"
#include <filesystem>
#include <fstream>

using namespace marketdata;

class MarketDataRecorderTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = std::filesystem::temp_directory_path() / "test_market_data";
        std::filesystem::create_directories(test_dir);

        recorder = std::make_unique<MarketDataRecorder>(
            test_dir.string(),
            RecordingFormat::BINARY,
            1000
        );
    }

    void TearDown() override {
        if (recorder) {
            recorder->stop();
        }

        // Cleanup test directory
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
    }

    NormalizedOrderBookUpdate create_update(const std::string& symbol, double bid_price, double ask_price) {
        NormalizedOrderBookUpdate update;
        update.symbol = symbol;
        update.exchange = Exchange::BINANCE;
        update.timestamp_ns = current_timestamp;
        update.is_snapshot = false;

        OrderBookLevel bid;
        bid.price = bid_price;
        bid.quantity = 1.0;
        bid.order_count = 1;
        update.bids.push_back(bid);

        OrderBookLevel ask;
        ask.price = ask_price;
        ask.quantity = 1.0;
        ask.order_count = 1;
        update.asks.push_back(ask);

        current_timestamp += 1000000;  // +1ms
        return update;
    }

    std::filesystem::path test_dir;
    std::unique_ptr<MarketDataRecorder> recorder;
    uint64_t current_timestamp = 1000000000;
};

// ============================================================================
// Basic Recording
// ============================================================================

TEST_F(MarketDataRecorderTest, InitialState) {
    auto stats = recorder->get_statistics();

    EXPECT_EQ(stats.total_updates_recorded, 0);
    EXPECT_EQ(stats.total_bytes_written, 0);
    EXPECT_FALSE(recorder->is_recording());
}

TEST_F(MarketDataRecorderTest, StartRecording) {
    EXPECT_TRUE(recorder->start());
    EXPECT_TRUE(recorder->is_recording());
}

TEST_F(MarketDataRecorderTest, StartAlreadyStarted) {
    recorder->start();

    // Starting again should fail
    EXPECT_FALSE(recorder->start());
}

TEST_F(MarketDataRecorderTest, StopRecording) {
    recorder->start();
    recorder->stop();

    EXPECT_FALSE(recorder->is_recording());
}

TEST_F(MarketDataRecorderTest, StopNotStarted) {
    // Stopping when not started should be safe
    EXPECT_NO_THROW({
        recorder->stop();
    });
}

// ============================================================================
// Recording Operations
// ============================================================================

TEST_F(MarketDataRecorderTest, RecordSingleUpdate) {
    recorder->start();

    auto update = create_update("BTCUSD", 42000.0, 42001.0);
    EXPECT_TRUE(recorder->record(update));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Allow async write

    recorder->stop();

    auto stats = recorder->get_statistics();
    EXPECT_EQ(stats.total_updates_recorded, 1);
    EXPECT_GT(stats.total_bytes_written, 0);
}

TEST_F(MarketDataRecorderTest, RecordMultipleUpdates) {
    recorder->start();

    for (int i = 0; i < 10; i++) {
        auto update = create_update("BTCUSD", 42000.0 + i, 42001.0 + i);
        recorder->record(update);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    recorder->stop();

    auto stats = recorder->get_statistics();
    EXPECT_EQ(stats.total_updates_recorded, 10);
}

TEST_F(MarketDataRecorderTest, RecordWithoutStart) {
    auto update = create_update("BTCUSD", 42000.0, 42001.0);

    // Recording without starting should fail
    EXPECT_FALSE(recorder->record(update));
}

TEST_F(MarketDataRecorderTest, RecordMultipleSymbols) {
    recorder->start();

    recorder->record(create_update("BTCUSD", 42000.0, 42001.0));
    recorder->record(create_update("ETHUSD", 2200.0, 2201.0));
    recorder->record(create_update("BNBUSD", 300.0, 301.0));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    recorder->stop();

    auto stats = recorder->get_statistics();
    EXPECT_EQ(stats.total_updates_recorded, 3);
}

// ============================================================================
// Statistics
// ============================================================================

TEST_F(MarketDataRecorderTest, StatisticsUpdates) {
    recorder->start();

    for (int i = 0; i < 100; i++) {
        recorder->record(create_update("BTCUSD", 42000.0, 42001.0));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto stats = recorder->get_statistics();

    EXPECT_EQ(stats.total_updates_recorded, 100);
    EXPECT_GT(stats.total_bytes_written, 0);
    EXPECT_GT(stats.updates_per_second, 0.0);

    std::cout << "Recorded 100 updates: " << stats.total_bytes_written << " bytes, "
              << stats.updates_per_second << " updates/sec\n";
}

// ============================================================================
// Different Formats
// ============================================================================

TEST(MarketDataRecorderFormatTest, BinaryFormat) {
    auto test_dir = std::filesystem::temp_directory_path() / "test_binary";
    std::filesystem::create_directories(test_dir);

    MarketDataRecorder recorder(test_dir.string(), RecordingFormat::BINARY, 1000);
    recorder.start();

    NormalizedOrderBookUpdate update;
    update.symbol = "BTCUSD";
    update.exchange = Exchange::BINANCE;
    update.timestamp_ns = 1000000000;
    recorder.record(update);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    recorder.stop();

    // Check that file was created
    bool found_file = false;
    for (const auto& entry : std::filesystem::directory_iterator(test_dir)) {
        if (entry.path().extension() == ".bin") {
            found_file = true;
            break;
        }
    }

    EXPECT_TRUE(found_file);

    std::filesystem::remove_all(test_dir);
}

TEST(MarketDataRecorderFormatTest, JSONFormat) {
    auto test_dir = std::filesystem::temp_directory_path() / "test_json";
    std::filesystem::create_directories(test_dir);

    MarketDataRecorder recorder(test_dir.string(), RecordingFormat::JSON, 1000);
    recorder.start();

    NormalizedOrderBookUpdate update;
    update.symbol = "BTCUSD";
    update.exchange = Exchange::BINANCE;
    update.timestamp_ns = 1000000000;
    recorder.record(update);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    recorder.stop();

    // Check for JSON file
    bool found_file = false;
    for (const auto& entry : std::filesystem::directory_iterator(test_dir)) {
        if (entry.path().extension() == ".json") {
            found_file = true;
            break;
        }
    }

    EXPECT_TRUE(found_file);

    std::filesystem::remove_all(test_dir);
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(MarketDataRecorderTest, PerformanceHighThroughput) {
    recorder->start();

    const int num_updates = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_updates; i++) {
        auto update = create_update("BTCUSD", 42000.0 + i, 42001.0 + i);
        recorder->record(update);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_updates;

    // Should be very fast (< 1000 ns per update for queue push)
    EXPECT_LT(avg_ns, 1000.0) << "Average record time: " << avg_ns << " ns";

    std::cout << "Recording avg: " << avg_ns << " ns per update\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // Allow writes to complete
    recorder->stop();

    auto stats = recorder->get_statistics();
    std::cout << "Recorded " << stats.total_updates_recorded << " updates, "
              << stats.total_bytes_written << " bytes\n";
}

// ============================================================================
// MarketDataReplayer Tests
// ============================================================================

class MarketDataReplayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = std::filesystem::temp_directory_path() / "test_replay";
        std::filesystem::create_directories(test_dir);

        // Record some data first
        recorder = std::make_unique<MarketDataRecorder>(
            test_dir.string(),
            RecordingFormat::BINARY,
            1000
        );

        recorder->start();

        current_timestamp = 1000000000;
        for (int i = 0; i < 10; i++) {
            NormalizedOrderBookUpdate update;
            update.symbol = "BTCUSD";
            update.exchange = Exchange::BINANCE;
            update.timestamp_ns = current_timestamp;
            current_timestamp += 10000000;  // +10ms

            OrderBookLevel level;
            level.price = 42000.0 + i;
            level.quantity = 1.0;
            update.bids.push_back(level);

            recorder->record(update);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        recorder->stop();

        // Find the recorded file
        for (const auto& entry : std::filesystem::directory_iterator(test_dir)) {
            if (entry.path().extension() == ".bin") {
                recorded_file = entry.path().string();
                break;
            }
        }
    }

    void TearDown() override {
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
    }

    std::filesystem::path test_dir;
    std::unique_ptr<MarketDataRecorder> recorder;
    std::string recorded_file;
    uint64_t current_timestamp;
};

TEST_F(MarketDataReplayerTest, LoadRecordedFile) {
    ASSERT_FALSE(recorded_file.empty());

    MarketDataReplayer replayer(recorded_file);

    // Should load successfully
    EXPECT_TRUE(replayer.is_loaded());
}

TEST_F(MarketDataReplayerTest, ReplayUpdates) {
    ASSERT_FALSE(recorded_file.empty());

    MarketDataReplayer replayer(recorded_file);

    int callback_count = 0;
    replayer.set_callback([&callback_count](const NormalizedOrderBookUpdate& update) {
        callback_count++;
        EXPECT_EQ(update.symbol, "BTCUSD");
        EXPECT_EQ(update.exchange, Exchange::BINANCE);
    });

    replayer.replay(10.0);  // Replay at 10x speed

    EXPECT_EQ(callback_count, 10);  // Should have replayed all 10 updates
}

TEST_F(MarketDataReplayerTest, ReplayFromTimestamp) {
    ASSERT_FALSE(recorded_file.empty());

    MarketDataReplayer replayer(recorded_file);

    int callback_count = 0;
    replayer.set_callback([&callback_count](const NormalizedOrderBookUpdate&) {
        callback_count++;
    });

    // Replay from middle (timestamp of 5th update)
    uint64_t start_time = 1000000000 + 5 * 10000000;
    replayer.replay_from(start_time, 10.0);

    // Should replay last 5 updates
    EXPECT_EQ(callback_count, 5);
}

TEST_F(MarketDataReplayerTest, ReplaySpeed) {
    ASSERT_FALSE(recorded_file.empty());

    MarketDataReplayer replayer(recorded_file);

    replayer.set_callback([](const NormalizedOrderBookUpdate&) {
        // Just consume
    });

    auto start = std::chrono::steady_clock::now();
    replayer.replay(10.0);  // 10x speed
    auto duration = std::chrono::steady_clock::now() - start;

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

    // 10 updates with 10ms spacing = 100ms real time
    // At 10x speed = ~10ms expected
    std::cout << "Replay took " << ms << " ms (10x speed)\n";

    // Should be reasonably fast
    EXPECT_LT(ms, 50);  // Allow some overhead
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(MarketDataRecorderTest, RecordAfterStop) {
    recorder->start();
    recorder->stop();

    auto update = create_update("BTCUSD", 42000.0, 42001.0);

    // Recording after stop should fail
    EXPECT_FALSE(recorder->record(update));
}

TEST_F(MarketDataRecorderTest, MultipleStartStop) {
    recorder->start();
    recorder->stop();
    recorder->start();
    recorder->stop();
    recorder->start();

    EXPECT_TRUE(recorder->is_recording());
}

TEST_F(MarketDataRecorderTest, EmptySymbol) {
    recorder->start();

    NormalizedOrderBookUpdate update;
    update.symbol = "";  // Empty symbol
    update.exchange = Exchange::BINANCE;
    update.timestamp_ns = 1000000000;

    // Should handle gracefully
    recorder->record(update);
}

// ============================================================================
// Concurrent Recording
// ============================================================================

TEST_F(MarketDataRecorderTest, ConcurrentRecording) {
    recorder->start();

    const int num_threads = 4;
    const int updates_per_thread = 100;

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, t, updates_per_thread]() {
            for (int i = 0; i < updates_per_thread; i++) {
                auto update = create_update(
                    "SYM" + std::to_string(t),
                    42000.0 + i,
                    42001.0 + i
                );
                recorder->record(update);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    recorder->stop();

    auto stats = recorder->get_statistics();
    EXPECT_EQ(stats.total_updates_recorded, num_threads * updates_per_thread);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
