#include <gtest/gtest.h>
#include "core/memory_pool.hpp"
#include <thread>
#include <vector>

using namespace marketdata;

struct TestObject {
    int value;
    double data;

    TestObject(int v, double d) : value(v), data(d) {}
};

TEST(MemoryPoolTest, AllocateAndDeallocate) {
    LockFreePool<TestObject, 100> pool;

    auto* obj = pool.allocate(42, 3.14);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->value, 42);
    EXPECT_DOUBLE_EQ(obj->data, 3.14);

    pool.deallocate(obj);
}

TEST(MemoryPoolTest, MultipleAllocations) {
    LockFreePool<int, 10> pool;

    std::vector<int*> ptrs;

    for (int i = 0; i < 10; ++i) {
        int* ptr = pool.allocate(i);
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }

    // Pool should be exhausted
    int* ptr = pool.allocate(99);
    EXPECT_EQ(ptr, nullptr);

    // Deallocate one
    pool.deallocate(ptrs[0]);

    // Should be able to allocate again
    ptr = pool.allocate(100);
    EXPECT_NE(ptr, nullptr);

    // Cleanup
    pool.deallocate(ptr);
    for (size_t i = 1; i < ptrs.size(); ++i) {
        pool.deallocate(ptrs[i]);
    }
}

TEST(MemoryPoolTest, ConcurrentAllocations) {
    LockFreePool<int, 1000> pool;
    const int num_threads = 10;
    const int allocations_per_thread = 50;

    std::vector<std::thread> threads;
    std::atomic<int> successful_allocations{0};

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&pool, &successful_allocations, allocations_per_thread, i]() {
            std::vector<int*> local_ptrs;

            for (int j = 0; j < allocations_per_thread; ++j) {
                int* ptr = pool.allocate(i * 1000 + j);
                if (ptr != nullptr) {
                    local_ptrs.push_back(ptr);
                    successful_allocations++;
                }
            }

            // Deallocate
            for (int* ptr : local_ptrs) {
                pool.deallocate(ptr);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(successful_allocations, num_threads * allocations_per_thread);
}

TEST(MemoryPoolTest, PoolPtr) {
    LockFreePool<TestObject, 100> pool;

    {
        PoolPtr<TestObject, 100> ptr(pool, pool.allocate(42, 3.14));

        ASSERT_TRUE(ptr);
        EXPECT_EQ(ptr->value, 42);
        EXPECT_DOUBLE_EQ(ptr->data, 3.14);

        // Object should be automatically deallocated when PoolPtr goes out of scope
    }

    // Pool should have space available again
    auto* obj = pool.allocate(99, 2.71);
    ASSERT_NE(obj, nullptr);
    pool.deallocate(obj);
}

TEST(MemoryPoolTest, Capacity) {
    LockFreePool<int, 50> pool;

    EXPECT_EQ(pool.capacity(), 50);
    EXPECT_EQ(pool.available(), 50);
    EXPECT_EQ(pool.allocated(), 0);

    int* ptr = pool.allocate(42);
    EXPECT_EQ(pool.allocated(), 1);
    EXPECT_EQ(pool.available(), 49);

    pool.deallocate(ptr);
    EXPECT_EQ(pool.allocated(), 0);
    EXPECT_EQ(pool.available(), 50);
}
