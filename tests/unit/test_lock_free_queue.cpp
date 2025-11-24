#include <gtest/gtest.h>
#include "utils/lock_free_queue.hpp"
#include <thread>
#include <vector>
#include <atomic>

using namespace marketdata;

TEST(LockFreeQueueTest, PushAndPop) {
    LockFreeQueue<int> queue;

    queue.push(1);
    queue.push(2);
    queue.push(3);

    auto val1 = queue.pop();
    auto val2 = queue.pop();
    auto val3 = queue.pop();

    ASSERT_TRUE(val1.has_value());
    ASSERT_TRUE(val2.has_value());
    ASSERT_TRUE(val3.has_value());

    EXPECT_EQ(*val1, 1);
    EXPECT_EQ(*val2, 2);
    EXPECT_EQ(*val3, 3);
}

TEST(LockFreeQueueTest, PopEmpty) {
    LockFreeQueue<int> queue;

    auto val = queue.pop();
    EXPECT_FALSE(val.has_value());
}

TEST(LockFreeQueueTest, ConcurrentPushPop) {
    LockFreeQueue<int> queue;
    const int num_items = 10000;
    std::atomic<int> push_count{0};
    std::atomic<int> pop_count{0};

    // Producer thread
    std::thread producer([&queue, &push_count, num_items]() {
        for (int i = 0; i < num_items; ++i) {
            queue.push(i);
            push_count++;
        }
    });

    // Consumer thread
    std::thread consumer([&queue, &pop_count, num_items]() {
        int popped = 0;
        while (popped < num_items) {
            auto val = queue.pop();
            if (val.has_value()) {
                popped++;
                pop_count++;
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(push_count, num_items);
    EXPECT_EQ(pop_count, num_items);
    EXPECT_TRUE(queue.empty());
}

TEST(LockFreeQueueTest, MultipleProducersConsumers) {
    LockFreeQueue<int> queue;
    const int num_threads = 4;
    const int items_per_thread = 1000;
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    // Start producers
    for (int i = 0; i < num_threads; ++i) {
        producers.emplace_back([&queue, &total_produced, items_per_thread]() {
            for (int j = 0; j < items_per_thread; ++j) {
                queue.push(j);
                total_produced++;
            }
        });
    }

    // Start consumers
    for (int i = 0; i < num_threads; ++i) {
        consumers.emplace_back([&queue, &total_consumed, items_per_thread, num_threads]() {
            int consumed = 0;
            while (consumed < items_per_thread) {
                auto val = queue.pop();
                if (val.has_value()) {
                    consumed++;
                    total_consumed++;
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(total_produced, num_threads * items_per_thread);
    EXPECT_EQ(total_consumed, num_threads * items_per_thread);
}
