/**
 * @file test_lock_free_queue.cpp
 * @brief Unit tests for LockFreeQueue
 */

#include <gtest/gtest.h>
#include "utils/lock_free_queue.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <set>

using namespace marketdata;

class LockFreeQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue = std::make_unique<LockFreeQueue<int>>();
    }

    std::unique_ptr<LockFreeQueue<int>> queue;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(LockFreeQueueTest, InitiallyEmpty) {
    auto value = queue->pop();
    EXPECT_FALSE(value.has_value());
}

TEST_F(LockFreeQueueTest, PushAndPop) {
    queue->push(42);

    auto value = queue->pop();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 42);
}

TEST_F(LockFreeQueueTest, MultipleElements) {
    queue->push(1);
    queue->push(2);
    queue->push(3);

    EXPECT_EQ(*queue->pop(), 1);
    EXPECT_EQ(*queue->pop(), 2);
    EXPECT_EQ(*queue->pop(), 3);
    EXPECT_FALSE(queue->pop().has_value());
}

TEST_F(LockFreeQueueTest, FIFOOrder) {
    const int count = 100;

    for (int i = 0; i < count; i++) {
        queue->push(i);
    }

    for (int i = 0; i < count; i++) {
        auto value = queue->pop();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }
}

TEST_F(LockFreeQueueTest, InterleavedPushPop) {
    queue->push(1);
    EXPECT_EQ(*queue->pop(), 1);

    queue->push(2);
    queue->push(3);
    EXPECT_EQ(*queue->pop(), 2);

    queue->push(4);
    EXPECT_EQ(*queue->pop(), 3);
    EXPECT_EQ(*queue->pop(), 4);
}

// ============================================================================
// Concurrency Tests - Multiple Producers
// ============================================================================

TEST_F(LockFreeQueueTest, MultipleProducers) {
    const int num_producers = 4;
    const int items_per_producer = 1000;

    std::vector<std::thread> producers;

    for (int p = 0; p < num_producers; p++) {
        producers.emplace_back([this, p, items_per_producer]() {
            for (int i = 0; i < items_per_producer; i++) {
                queue->push(p * 10000 + i);
            }
        });
    }

    for (auto& thread : producers) {
        thread.join();
    }

    // Consume all items
    std::set<int> consumed;
    while (auto value = queue->pop()) {
        consumed.insert(*value);
    }

    // Should have all items (no duplicates, no losses)
    EXPECT_EQ(consumed.size(), num_producers * items_per_producer);
}

// ============================================================================
// Concurrency Tests - Multiple Consumers
// ============================================================================

TEST_F(LockFreeQueueTest, MultipleConsumers) {
    const int num_items = 10000;
    const int num_consumers = 4;

    // Push all items
    for (int i = 0; i < num_items; i++) {
        queue->push(i);
    }

    std::vector<std::thread> consumers;
    std::vector<std::set<int>> consumed(num_consumers);

    for (int c = 0; c < num_consumers; c++) {
        consumers.emplace_back([this, c, &consumed]() {
            while (auto value = queue->pop()) {
                consumed[c].insert(*value);
            }
        });
    }

    for (auto& thread : consumers) {
        thread.join();
    }

    // Merge all consumed items
    std::set<int> all_consumed;
    for (const auto& set : consumed) {
        all_consumed.insert(set.begin(), set.end());
    }

    // Should have all items (no duplicates, no losses)
    EXPECT_EQ(all_consumed.size(), num_items);
}

// ============================================================================
// Concurrency Tests - Multiple Producers and Consumers
// ============================================================================

TEST_F(LockFreeQueueTest, MultipleProducersAndConsumers) {
    const int num_producers = 4;
    const int num_consumers = 4;
    const int items_per_producer = 1000;

    std::vector<std::thread> threads;
    std::vector<std::set<int>> consumed(num_consumers);
    std::atomic<bool> done{false};

    // Start producers
    for (int p = 0; p < num_producers; p++) {
        threads.emplace_back([this, p, items_per_producer]() {
            for (int i = 0; i < items_per_producer; i++) {
                queue->push(p * 10000 + i);
            }
        });
    }

    // Start consumers
    for (int c = 0; c < num_consumers; c++) {
        threads.emplace_back([this, c, &consumed, &done]() {
            while (!done.load()) {
                while (auto value = queue->pop()) {
                    consumed[c].insert(*value);
                }
                std::this_thread::yield();
            }

            // Final drain
            while (auto value = queue->pop()) {
                consumed[c].insert(*value);
            }
        });
    }

    // Wait for producers
    for (int i = 0; i < num_producers; i++) {
        threads[i].join();
    }

    // Allow consumers to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    done.store(true);

    // Wait for consumers
    for (int i = num_producers; i < num_producers + num_consumers; i++) {
        threads[i].join();
    }

    // Merge all consumed items
    std::set<int> all_consumed;
    for (const auto& set : consumed) {
        all_consumed.insert(set.begin(), set.end());
    }

    // Should have all items
    EXPECT_EQ(all_consumed.size(), num_producers * items_per_producer);
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST_F(LockFreeQueueTest, HighContentionStress) {
    const int num_threads = 8;
    const int operations_per_thread = 10000;

    std::vector<std::thread> threads;
    std::atomic<int> total_pushed{0};
    std::atomic<int> total_popped{0};

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, t, operations_per_thread, &total_pushed, &total_popped]() {
            for (int i = 0; i < operations_per_thread; i++) {
                // Alternate push/pop
                if ((t + i) % 2 == 0) {
                    queue->push(t * 100000 + i);
                    total_pushed++;
                } else {
                    if (queue->pop().has_value()) {
                        total_popped++;
                    }
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Drain remaining items
    while (queue->pop().has_value()) {
        total_popped++;
    }

    // All pushed items should be popped
    EXPECT_EQ(total_pushed.load(), total_popped.load());
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(LockFreeQueueTest, PerformanceSingleThreaded) {
    const int num_operations = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_operations; i++) {
        queue->push(i);
    }

    for (int i = 0; i < num_operations; i++) {
        queue->pop();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / (num_operations * 2);

    // Should be sub-microsecond
    EXPECT_LT(avg_ns, 500.0) << "Average operation time: " << avg_ns << " ns";

    std::cout << "Single-threaded avg operation time: " << avg_ns << " ns\n";
}

TEST_F(LockFreeQueueTest, PerformanceMultiThreaded) {
    const int num_threads = 4;
    const int operations_per_thread = 100000;

    std::vector<std::thread> threads;

    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, operations_per_thread]() {
            for (int i = 0; i < operations_per_thread; i++) {
                queue->push(i);
                queue->pop();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / (num_threads * operations_per_thread * 2);

    std::cout << "Multi-threaded avg operation time: " << avg_ns << " ns\n";
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(LockFreeQueueTest, RapidPushPop) {
    // Rapidly push and pop
    for (int i = 0; i < 1000; i++) {
        queue->push(i);
        auto value = queue->pop();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }
}

TEST_F(LockFreeQueueTest, LargeDataStructure) {
    struct LargeStruct {
        char data[1024];
        int id;
    };

    LockFreeQueue<LargeStruct> large_queue;

    LargeStruct item;
    item.id = 42;

    large_queue.push(item);

    auto result = large_queue.pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, 42);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
