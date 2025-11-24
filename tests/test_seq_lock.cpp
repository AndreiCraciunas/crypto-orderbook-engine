/**
 * @file test_seq_lock.cpp
 * @brief Unit tests for SeqLock (Sequence Lock)
 */

#include <gtest/gtest.h>
#include "utils/seq_lock.hpp"
#include <thread>
#include <vector>
#include <atomic>

using namespace marketdata;

// Test data structure
struct TestData {
    int value1;
    double value2;
    uint64_t value3;

    bool operator==(const TestData& other) const {
        return value1 == other.value1 &&
               value2 == other.value2 &&
               value3 == other.value3;
    }
};

class SeqLockTest : public ::testing::Test {
protected:
    void SetUp() override {
        seq_lock = std::make_unique<SeqLock<TestData>>();
    }

    std::unique_ptr<SeqLock<TestData>> seq_lock;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(SeqLockTest, InitialRead) {
    TestData data = seq_lock->read();

    // Should return default-initialized data
    EXPECT_EQ(data.value1, 0);
    EXPECT_DOUBLE_EQ(data.value2, 0.0);
    EXPECT_EQ(data.value3, 0);
}

TEST_F(SeqLockTest, WriteAndRead) {
    TestData write_data{42, 3.14, 1000000};

    seq_lock->write(write_data);

    TestData read_data = seq_lock->read();

    EXPECT_EQ(read_data, write_data);
}

TEST_F(SeqLockTest, MultipleWrites) {
    TestData data1{1, 1.1, 100};
    TestData data2{2, 2.2, 200};
    TestData data3{3, 3.3, 300};

    seq_lock->write(data1);
    seq_lock->write(data2);
    seq_lock->write(data3);

    TestData final = seq_lock->read();

    EXPECT_EQ(final, data3);  // Should have latest write
}

TEST_F(SeqLockTest, ReadStability) {
    TestData data{42, 3.14, 1000000};
    seq_lock->write(data);

    // Multiple reads should return same data
    TestData read1 = seq_lock->read();
    TestData read2 = seq_lock->read();
    TestData read3 = seq_lock->read();

    EXPECT_EQ(read1, data);
    EXPECT_EQ(read2, data);
    EXPECT_EQ(read3, data);
}

// ============================================================================
// Consistency Tests
// ============================================================================

TEST_F(SeqLockTest, ReadConsistencyDuringWrite) {
    std::atomic<bool> stop{false};
    std::atomic<int> inconsistent_reads{0};
    std::atomic<int> total_reads{0};

    // Writer thread
    std::thread writer([this, &stop]() {
        int counter = 0;
        while (!stop.load()) {
            TestData data{counter, static_cast<double>(counter), static_cast<uint64_t>(counter)};
            seq_lock->write(data);
            counter++;
        }
    });

    // Reader threads
    const int num_readers = 4;
    std::vector<std::thread> readers;

    for (int i = 0; i < num_readers; i++) {
        readers.emplace_back([this, &stop, &inconsistent_reads, &total_reads]() {
            while (!stop.load()) {
                TestData data = seq_lock->read();
                total_reads++;

                // Check consistency: all values should be equal
                if (data.value1 != static_cast<int>(data.value2) ||
                    data.value1 != static_cast<int>(data.value3)) {
                    inconsistent_reads++;
                }
            }
        });
    }

    // Run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);

    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }

    // Should have NO inconsistent reads
    EXPECT_EQ(inconsistent_reads.load(), 0);
    EXPECT_GT(total_reads.load(), 0);

    std::cout << "Read " << total_reads.load() << " times with 0 inconsistencies\n";
}

TEST_F(SeqLockTest, NoTearingReads) {
    // Ensure reads are atomic (no partial reads)
    std::atomic<bool> stop{false};

    std::thread writer([this, &stop]() {
        uint64_t counter = 0;
        while (!stop.load()) {
            TestData data{
                static_cast<int>(counter),
                static_cast<double>(counter),
                counter
            };
            seq_lock->write(data);
            counter++;
        }
    });

    std::thread reader([this, &stop]() {
        while (!stop.load()) {
            TestData data = seq_lock->read();

            // All fields should be from the same write
            EXPECT_EQ(data.value1, static_cast<int>(data.value3));
            EXPECT_DOUBLE_EQ(data.value2, static_cast<double>(data.value3));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);

    writer.join();
    reader.join();
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST_F(SeqLockTest, MultipleReaders) {
    TestData write_data{100, 200.5, 300};
    seq_lock->write(write_data);

    const int num_readers = 8;
    const int reads_per_reader = 10000;

    std::vector<std::thread> readers;
    std::atomic<int> successful_reads{0};

    for (int i = 0; i < num_readers; i++) {
        readers.emplace_back([this, write_data, reads_per_reader, &successful_reads]() {
            for (int j = 0; j < reads_per_reader; j++) {
                TestData data = seq_lock->read();
                if (data == write_data) {
                    successful_reads++;
                }
            }
        });
    }

    for (auto& reader : readers) {
        reader.join();
    }

    // All reads should match the written data
    EXPECT_EQ(successful_reads.load(), num_readers * reads_per_reader);
}

TEST_F(SeqLockTest, SingleWriterMultipleReaders) {
    const int num_readers = 4;
    const int num_writes = 1000;

    std::atomic<bool> writer_done{false};
    std::atomic<int> total_reads{0};

    // Single writer
    std::thread writer([this, num_writes, &writer_done]() {
        for (int i = 0; i < num_writes; i++) {
            TestData data{i, static_cast<double>(i), static_cast<uint64_t>(i)};
            seq_lock->write(data);
        }
        writer_done.store(true);
    });

    // Multiple readers
    std::vector<std::thread> readers;
    for (int i = 0; i < num_readers; i++) {
        readers.emplace_back([this, &writer_done, &total_reads]() {
            while (!writer_done.load()) {
                TestData data = seq_lock->read();
                total_reads++;

                // Verify consistency
                EXPECT_EQ(data.value1, static_cast<int>(data.value2));
                EXPECT_EQ(data.value1, static_cast<int>(data.value3));
            }
        });
    }

    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }

    EXPECT_GT(total_reads.load(), 0);
    std::cout << "Total reads: " << total_reads.load() << "\n";
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(SeqLockTest, PerformanceWrite) {
    const int num_writes = 1000000;
    TestData data{42, 3.14, 1000000};

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_writes; i++) {
        data.value1 = i;
        seq_lock->write(data);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_writes;

    // Should be very fast (< 50 ns)
    EXPECT_LT(avg_ns, 50.0) << "Average write time: " << avg_ns << " ns";

    std::cout << "SeqLock write avg: " << avg_ns << " ns\n";
}

TEST_F(SeqLockTest, PerformanceRead) {
    TestData data{42, 3.14, 1000000};
    seq_lock->write(data);

    const int num_reads = 1000000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_reads; i++) {
        TestData read_data = seq_lock->read();
        volatile int val = read_data.value1;  // Prevent optimization
        (void)val;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_reads;

    // Should be very fast (< 100 ns)
    EXPECT_LT(avg_ns, 100.0) << "Average read time: " << avg_ns << " ns";

    std::cout << "SeqLock read avg: " << avg_ns << " ns\n";
}

TEST_F(SeqLockTest, PerformanceConcurrentReads) {
    TestData data{42, 3.14, 1000000};
    seq_lock->write(data);

    const int num_readers = 4;
    const int reads_per_reader = 250000;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> readers;
    for (int i = 0; i < num_readers; i++) {
        readers.emplace_back([this, reads_per_reader]() {
            for (int j = 0; j < reads_per_reader; j++) {
                TestData read_data = seq_lock->read();
                volatile int val = read_data.value1;
                (void)val;
            }
        });
    }

    for (auto& reader : readers) {
        reader.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / (num_readers * reads_per_reader);

    std::cout << "SeqLock concurrent read avg: " << avg_ns << " ns\n";
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(SeqLockTest, RapidWrites) {
    // Write as fast as possible
    for (int i = 0; i < 10000; i++) {
        TestData data{i, static_cast<double>(i), static_cast<uint64_t>(i)};
        seq_lock->write(data);
    }

    // Final read should be consistent
    TestData final = seq_lock->read();
    EXPECT_EQ(final.value1, static_cast<int>(final.value2));
    EXPECT_EQ(final.value1, static_cast<int>(final.value3));
}

TEST_F(SeqLockTest, InterleavedWritesAndReads) {
    for (int i = 0; i < 1000; i++) {
        TestData write_data{i, static_cast<double>(i), static_cast<uint64_t>(i)};
        seq_lock->write(write_data);

        TestData read_data = seq_lock->read();

        // Read should be consistent (might be old or new value)
        EXPECT_EQ(read_data.value1, static_cast<int>(read_data.value2));
        EXPECT_EQ(read_data.value1, static_cast<int>(read_data.value3));
    }
}

// ============================================================================
// Large Data Structure
// ============================================================================

struct LargeData {
    char buffer[1024];
    uint64_t checksum;

    bool is_valid() const {
        uint64_t sum = 0;
        for (char c : buffer) {
            sum += static_cast<uint8_t>(c);
        }
        return sum == checksum;
    }
};

TEST(SeqLockLargeDataTest, LargeStructure) {
    SeqLock<LargeData> large_lock;

    LargeData data;
    std::fill(std::begin(data.buffer), std::end(data.buffer), 'A');
    data.checksum = 1024 * 'A';

    large_lock.write(data);

    LargeData read_data = large_lock.read();

    EXPECT_TRUE(read_data.is_valid());
}

// ============================================================================
// Real-World Usage
// ============================================================================

struct OrderBookSnapshot {
    double best_bid;
    double best_ask;
    uint64_t timestamp_ns;
    int bid_count;
    int ask_count;

    bool is_consistent() const {
        return best_bid < best_ask && bid_count >= 0 && ask_count >= 0;
    }
};

TEST(SeqLockRealWorldTest, OrderBookUsage) {
    SeqLock<OrderBookSnapshot> snapshot_lock;

    std::atomic<bool> stop{false};
    std::atomic<int> inconsistent_snapshots{0};

    // Simulated order book updater
    std::thread updater([&snapshot_lock, &stop]() {
        double base_price = 42000.0;
        int counter = 0;

        while (!stop.load()) {
            OrderBookSnapshot snapshot;
            snapshot.best_bid = base_price + counter;
            snapshot.best_ask = base_price + counter + 1.0;
            snapshot.timestamp_ns = counter * 1000;
            snapshot.bid_count = 10 + counter % 5;
            snapshot.ask_count = 8 + counter % 5;

            snapshot_lock.write(snapshot);
            counter++;

            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    // Simulated readers
    std::thread reader([&snapshot_lock, &stop, &inconsistent_snapshots]() {
        while (!stop.load()) {
            OrderBookSnapshot snapshot = snapshot_lock.read();

            if (!snapshot.is_consistent()) {
                inconsistent_snapshots++;
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);

    updater.join();
    reader.join();

    // Should never read inconsistent data
    EXPECT_EQ(inconsistent_snapshots.load(), 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
