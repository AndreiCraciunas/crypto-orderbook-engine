/**
 * @file test_memory_pool.cpp
 * @brief Unit tests for LockFreePool
 */

#include <gtest/gtest.h>
#include "core/memory_pool.hpp"
#include <thread>
#include <vector>
#include <set>

using namespace marketdata;

struct TestObject {
    int id;
    double value;
    char data[64];

    TestObject() : id(0), value(0.0) {}
    TestObject(int i, double v) : id(i), value(v) {}
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST(MemoryPoolTest, AllocateSingle) {
    LockFreePool<TestObject, 100> pool;

    TestObject* obj = pool.allocate(42, 3.14);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->id, 42);
    EXPECT_DOUBLE_EQ(obj->value, 3.14);

    pool.deallocate(obj);
}

TEST(MemoryPoolTest, AllocateMultiple) {
    LockFreePool<TestObject, 100> pool;

    std::vector<TestObject*> objects;

    for (int i = 0; i < 10; i++) {
        TestObject* obj = pool.allocate(i, i * 1.5);
        ASSERT_NE(obj, nullptr);
        objects.push_back(obj);
    }

    // Verify all objects
    for (size_t i = 0; i < objects.size(); i++) {
        EXPECT_EQ(objects[i]->id, static_cast<int>(i));
        EXPECT_DOUBLE_EQ(objects[i]->value, i * 1.5);
    }

    // Deallocate all
    for (auto* obj : objects) {
        pool.deallocate(obj);
    }
}

TEST(MemoryPoolTest, ReuseAfterDeallocate) {
    LockFreePool<TestObject, 100> pool;

    TestObject* obj1 = pool.allocate(1, 1.0);
    void* addr1 = obj1;

    pool.deallocate(obj1);

    TestObject* obj2 = pool.allocate(2, 2.0);
    void* addr2 = obj2;

    // Should reuse the same slot
    EXPECT_EQ(addr1, addr2);
    EXPECT_EQ(obj2->id, 2);
}

// ============================================================================
// Pool Exhaustion
// ============================================================================

TEST(MemoryPoolTest, PoolExhaustion) {
    LockFreePool<TestObject, 10> pool;

    std::vector<TestObject*> objects;

    // Allocate all slots
    for (int i = 0; i < 10; i++) {
        TestObject* obj = pool.allocate(i, i * 1.0);
        ASSERT_NE(obj, nullptr);
        objects.push_back(obj);
    }

    // Next allocation should fail
    TestObject* overflow = pool.allocate(99, 99.0);
    EXPECT_EQ(overflow, nullptr);

    // Deallocate one
    pool.deallocate(objects[0]);

    // Now should succeed
    TestObject* reused = pool.allocate(100, 100.0);
    EXPECT_NE(reused, nullptr);

    // Cleanup
    for (size_t i = 1; i < objects.size(); i++) {
        pool.deallocate(objects[i]);
    }
    pool.deallocate(reused);
}

// ============================================================================
// PoolPtr RAII Tests
// ============================================================================

TEST(MemoryPoolTest, PoolPtrRAII) {
    LockFreePool<TestObject, 100> pool;

    {
        PoolPtr<TestObject, 100> ptr1(&pool, 42, 3.14);

        EXPECT_NE(ptr1.get(), nullptr);
        EXPECT_EQ(ptr1->id, 42);
        EXPECT_DOUBLE_EQ(ptr1->value, 3.14);

        // Test operator*
        EXPECT_EQ((*ptr1).id, 42);

    } // ptr1 destroyed, should auto-deallocate

    // Allocate again - should reuse
    PoolPtr<TestObject, 100> ptr2(&pool, 100, 200.0);
    EXPECT_NE(ptr2.get(), nullptr);
}

TEST(MemoryPoolTest, PoolPtrMove) {
    LockFreePool<TestObject, 100> pool;

    PoolPtr<TestObject, 100> ptr1(&pool, 42, 3.14);
    void* addr1 = ptr1.get();

    // Move construction
    PoolPtr<TestObject, 100> ptr2(std::move(ptr1));

    EXPECT_EQ(ptr1.get(), nullptr);  // ptr1 should be null after move
    EXPECT_EQ(ptr2.get(), addr1);    // ptr2 should have the object
    EXPECT_EQ(ptr2->id, 42);
}

TEST(MemoryPoolTest, PoolPtrMoveAssignment) {
    LockFreePool<TestObject, 100> pool;

    PoolPtr<TestObject, 100> ptr1(&pool, 42, 3.14);
    PoolPtr<TestObject, 100> ptr2(&pool, 100, 200.0);

    void* addr1 = ptr1.get();

    // Move assignment
    ptr2 = std::move(ptr1);

    EXPECT_EQ(ptr1.get(), nullptr);
    EXPECT_EQ(ptr2.get(), addr1);
    EXPECT_EQ(ptr2->id, 42);
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST(MemoryPoolTest, ConcurrentAllocations) {
    LockFreePool<TestObject, 1000> pool;
    const int num_threads = 8;
    const int allocs_per_thread = 100;

    std::vector<std::thread> threads;
    std::vector<std::vector<TestObject*>> thread_objects(num_threads);

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&pool, &thread_objects, t, allocs_per_thread]() {
            for (int i = 0; i < allocs_per_thread; i++) {
                TestObject* obj = pool.allocate(t * 1000 + i, i * 1.0);
                if (obj) {
                    thread_objects[t].push_back(obj);
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Count total allocations
    size_t total = 0;
    std::set<void*> unique_addresses;

    for (const auto& objects : thread_objects) {
        total += objects.size();
        for (auto* obj : objects) {
            unique_addresses.insert(obj);
        }
    }

    // Should have no duplicate addresses
    EXPECT_EQ(total, unique_addresses.size());

    // Deallocate all
    for (auto& objects : thread_objects) {
        for (auto* obj : objects) {
            pool.deallocate(obj);
        }
    }
}

TEST(MemoryPoolTest, ConcurrentAllocDeallocStress) {
    LockFreePool<TestObject, 1000> pool;
    const int num_threads = 8;
    const int operations_per_thread = 1000;

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&pool, operations_per_thread]() {
            std::vector<TestObject*> local_objects;

            for (int i = 0; i < operations_per_thread; i++) {
                // Allocate
                TestObject* obj = pool.allocate(i, i * 1.0);
                if (obj) {
                    local_objects.push_back(obj);
                }

                // Periodically deallocate
                if (!local_objects.empty() && i % 10 == 0) {
                    pool.deallocate(local_objects.back());
                    local_objects.pop_back();
                }
            }

            // Cleanup
            for (auto* obj : local_objects) {
                pool.deallocate(obj);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST(MemoryPoolTest, PerformanceAllocate) {
    LockFreePool<TestObject, 10000> pool;
    const int num_operations = 10000;

    std::vector<TestObject*> objects;
    objects.reserve(num_operations);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_operations; i++) {
        TestObject* obj = pool.allocate(i, i * 1.0);
        objects.push_back(obj);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / num_operations;

    // Should be very fast (< 100 ns)
    EXPECT_LT(avg_ns, 100.0) << "Average allocation time: " << avg_ns << " ns";

    std::cout << "Pool allocation avg: " << avg_ns << " ns\n";

    // Cleanup
    for (auto* obj : objects) {
        pool.deallocate(obj);
    }
}

TEST(MemoryPoolTest, PerformanceVsMalloc) {
    LockFreePool<TestObject, 10000> pool;
    const int num_operations = 10000;

    // Test pool
    auto pool_start = std::chrono::high_resolution_clock::now();
    std::vector<TestObject*> pool_objects;
    for (int i = 0; i < num_operations; i++) {
        pool_objects.push_back(pool.allocate(i, i * 1.0));
    }
    for (auto* obj : pool_objects) {
        pool.deallocate(obj);
    }
    auto pool_end = std::chrono::high_resolution_clock::now();

    // Test malloc
    auto malloc_start = std::chrono::high_resolution_clock::now();
    std::vector<TestObject*> malloc_objects;
    for (int i = 0; i < num_operations; i++) {
        malloc_objects.push_back(new TestObject(i, i * 1.0));
    }
    for (auto* obj : malloc_objects) {
        delete obj;
    }
    auto malloc_end = std::chrono::high_resolution_clock::now();

    auto pool_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(pool_end - pool_start).count();
    auto malloc_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(malloc_end - malloc_start).count();

    double pool_avg = static_cast<double>(pool_duration) / (num_operations * 2);
    double malloc_avg = static_cast<double>(malloc_duration) / (num_operations * 2);

    std::cout << "Pool avg: " << pool_avg << " ns, malloc avg: " << malloc_avg << " ns\n";
    std::cout << "Speedup: " << (malloc_avg / pool_avg) << "x\n";

    // Pool should be faster than malloc
    EXPECT_LT(pool_avg, malloc_avg);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(MemoryPoolTest, DoubleDeallocate) {
    LockFreePool<TestObject, 100> pool;

    TestObject* obj = pool.allocate(42, 3.14);
    ASSERT_NE(obj, nullptr);

    pool.deallocate(obj);

    // Double deallocate - should be handled gracefully
    // (Implementation should mark slot as free only once)
    pool.deallocate(obj);
}

TEST(MemoryPoolTest, NullDeallocate) {
    LockFreePool<TestObject, 100> pool;

    // Should handle null gracefully
    EXPECT_NO_THROW({
        pool.deallocate(nullptr);
    });
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
