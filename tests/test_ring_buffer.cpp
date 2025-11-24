/**
 * @file test_ring_buffer.cpp
 * @brief Unit tests for RingBuffer (SPSC)
 */

#include <gtest/gtest.h>
#include "utils/ring_buffer.hpp"
#include <thread>

using namespace marketdata;

// ============================================================================
// Basic Operations
// ============================================================================

TEST(RingBufferTest, InitiallyEmpty) {
    RingBuffer<int, 16> buffer;

    EXPECT_FALSE(buffer.pop().has_value());
}

TEST(RingBufferTest, PushAndPop) {
    RingBuffer<int, 16> buffer;

    EXPECT_TRUE(buffer.push(42));

    auto value = buffer.pop();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 42);
}

TEST(RingBufferTest, MultipleElements) {
    RingBuffer<int, 16> buffer;

    buffer.push(1);
    buffer.push(2);
    buffer.push(3);

    EXPECT_EQ(*buffer.pop(), 1);
    EXPECT_EQ(*buffer.pop(), 2);
    EXPECT_EQ(*buffer.pop(), 3);
    EXPECT_FALSE(buffer.pop().has_value());
}

TEST(RingBufferTest, FIFOOrder) {
    RingBuffer<int, 64> buffer;

    for (int i = 0; i < 50; i++) {
        buffer.push(i);
    }

    for (int i = 0; i < 50; i++) {
        auto value = buffer.pop();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }
}

// ============================================================================
// Capacity Tests
// ============================================================================

TEST(RingBufferTest, FillToCapacity) {
    RingBuffer<int, 16> buffer;

    // Can push 15 items (capacity - 1)
    for (int i = 0; i < 15; i++) {
        EXPECT_TRUE(buffer.push(i));
    }

    // 16th push should fail (full)
    EXPECT_FALSE(buffer.push(99));
}

TEST(RingBufferTest, WrapAround) {
    RingBuffer<int, 8> buffer;

    // Fill buffer
    for (int i = 0; i < 7; i++) {
        buffer.push(i);
    }

    // Pop some
    buffer.pop();
    buffer.pop();
    buffer.pop();

    // Push more (should wrap around)
    EXPECT_TRUE(buffer.push(100));
    EXPECT_TRUE(buffer.push(101));
    EXPECT_TRUE(buffer.push(102));

    // Check order
    EXPECT_EQ(*buffer.pop(), 3);
    EXPECT_EQ(*buffer.pop(), 4);
    EXPECT_EQ(*buffer.pop(), 5);
    EXPECT_EQ(*buffer.pop(), 6);
    EXPECT_EQ(*buffer.pop(), 100);
    EXPECT_EQ(*buffer.pop(), 101);
    EXPECT_EQ(*buffer.pop(), 102);
}

// ============================================================================
// SPSC Concurrency Test
// ============================================================================

TEST(RingBufferTest, SPSCProducerConsumer) {
    RingBuffer<int, 1024> buffer;
    const int num_items = 100000;

    std::atomic<bool> producer_done{false};

    // Producer thread
    std::thread producer([&buffer, num_items, &producer_done]() {
        for (int i = 0; i < num_items; i++) {
            while (!buffer.push(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true);
    });

    // Consumer thread
    std::thread consumer([&buffer, num_items, &producer_done]() {
        int expected = 0;
        while (expected < num_items) {
            auto value = buffer.pop();
            if (value.has_value()) {
                EXPECT_EQ(*value, expected);
                expected++;
            } else if (producer_done.load()) {
                // Final drain
                while (auto v = buffer.pop()) {
                    EXPECT_EQ(*v, expected);
                    expected++;
                }
                break;
            } else {
                std::this_thread::yield();
            }
        }
        EXPECT_EQ(expected, num_items);
    });

    producer.join();
    consumer.join();
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST(RingBufferTest, PerformanceSingleThreaded) {
    RingBuffer<int, 1024> buffer;
    const int num_operations = 1000000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_operations; i++) {
        while (!buffer.push(i)) {}
        buffer.pop();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / (num_operations * 2);

    // Should be very fast (< 50 ns)
    EXPECT_LT(avg_ns, 50.0) << "Average operation time: " << avg_ns << " ns";

    std::cout << "RingBuffer single-threaded avg: " << avg_ns << " ns\n";
}

TEST(RingBufferTest, PerformanceSPSC) {
    RingBuffer<int, 2048> buffer;
    const int num_items = 1000000;

    auto start = std::chrono::high_resolution_clock::now();

    std::thread producer([&buffer, num_items]() {
        for (int i = 0; i < num_items; i++) {
            while (!buffer.push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&buffer, num_items]() {
        for (int i = 0; i < num_items; i++) {
            while (true) {
                auto value = buffer.pop();
                if (value.has_value()) {
                    break;
                }
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_items;

    std::cout << "RingBuffer SPSC avg latency: " << avg_ns << " ns per item\n";
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(RingBufferTest, AlternatingPushPop) {
    RingBuffer<int, 16> buffer;

    for (int i = 0; i < 1000; i++) {
        EXPECT_TRUE(buffer.push(i));
        auto value = buffer.pop();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }

    // Should be empty
    EXPECT_FALSE(buffer.pop().has_value());
}

TEST(RingBufferTest, PowerOfTwoCapacity) {
    // Buffer requires power-of-2 capacity
    // This should compile fine
    RingBuffer<int, 16> buf16;
    RingBuffer<int, 32> buf32;
    RingBuffer<int, 64> buf64;
    RingBuffer<int, 1024> buf1024;

    (void)buf16;
    (void)buf32;
    (void)buf64;
    (void)buf1024;
}

TEST(RingBufferTest, LargeDataStructure) {
    struct LargeStruct {
        char data[256];
        int id;
    };

    RingBuffer<LargeStruct, 16> buffer;

    LargeStruct item;
    item.id = 123;

    EXPECT_TRUE(buffer.push(item));

    auto result = buffer.pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, 123);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
