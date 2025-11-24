/**
 * @file memory_pool.hpp
 * @brief Lock-free memory pool for zero-allocation hot path
 *
 * Provides a pre-allocated object pool that eliminates dynamic memory
 * allocation in performance-critical code paths. Uses atomic operations
 * for thread-safe allocation and deallocation without locks.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>

namespace marketdata {

/**
 * @brief Lock-free memory pool for zero-allocation hot path
 *
 * Pre-allocates a fixed number of objects at startup, allowing lock-free
 * allocation and deallocation during runtime. Designed to eliminate heap
 * allocations in latency-sensitive code paths.
 *
 * **Algorithm:**
 * - Pre-allocates aligned storage for POOL_SIZE objects
 * - Uses atomic boolean array to track slot availability
 * - Linear probing from head position to find free slots
 * - CAS operation to claim slots atomically
 * - In-place construction with placement new
 *
 * **Performance:**
 * - Allocate: O(N) worst case, O(1) typical (~50-100 ns)
 * - Deallocate: O(1), ~20-30 ns
 * - Zero heap allocations after initialization
 * - Lock-free for concurrent allocations
 *
 * **Memory Management:**
 * - Objects constructed in-place with placement new
 * - Destructor called explicitly on deallocation
 * - Pool owns all storage, no external memory
 * - Returns nullptr when pool exhausted
 *
 * @tparam T Object type to pool (must be movable/copyable)
 * @tparam POOL_SIZE Maximum number of objects (default: 100000)
 *
 * @code
 * // Create pool for PriceLevel updates
 * LockFreePool<PriceLevel, 10000> pool;
 *
 * // Allocate object
 * PriceLevel* level = pool.allocate(price, quantity, timestamp);
 * if (level) {
 *     // Use object
 *     process_level(level);
 *
 *     // Deallocate when done
 *     pool.deallocate(level);
 * }
 *
 * // Better: Use PoolPtr RAII wrapper
 * auto level_ptr = PoolPtr(pool, pool.allocate(price, qty, ts));
 * if (level_ptr) {
 *     process_level(level_ptr.get());
 * }  // Automatically deallocated
 * @endcode
 *
 * @note Thread-safe for multiple concurrent allocators/deallocators
 * @note Returns nullptr when pool is exhausted (no blocking)
 * @warning Fixed capacity - monitor allocated() to avoid exhaustion
 * @see PoolPtr for RAII management
 */
template<typename T, size_t POOL_SIZE = 100000>
class LockFreePool {
public:
    /**
     * @brief Construct empty pool
     *
     * Allocates storage for POOL_SIZE objects and initializes
     * all slots as available. No objects are constructed yet.
     */
    LockFreePool()
        : head_(0) {
        // Initialize all slots as not in use
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            in_use_[i].store(false, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Destructor
     *
     * Destroys all objects that are still allocated. Not thread-safe -
     * ensure all threads have finished using the pool before destruction.
     *
     * @warning Call only when no other threads are using the pool
     */
    ~LockFreePool() {
        // Destroy all objects that are still in use
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            if (in_use_[i].load(std::memory_order_acquire)) {
                reinterpret_cast<T*>(&storage_[i])->~T();
            }
        }
    }

    // Non-copyable, non-movable
    LockFreePool(const LockFreePool&) = delete;
    LockFreePool& operator=(const LockFreePool&) = delete;
    LockFreePool(LockFreePool&&) = delete;
    LockFreePool& operator=(LockFreePool&&) = delete;

    /**
     * @brief Allocate and construct object
     *
     * Finds a free slot using linear probing, claims it atomically,
     * and constructs object in-place using provided arguments.
     * Returns nullptr if pool is exhausted.
     *
     * @tparam Args Constructor argument types
     * @param args Arguments forwarded to T's constructor
     * @return T* Pointer to constructed object, or nullptr if pool full
     *
     * @code
     * LockFreePool<OrderBookSnapshot, 1000> pool;
     *
     * // Allocate with constructor arguments
     * auto* snapshot = pool.allocate(
     *     Exchange::BINANCE,
     *     "BTC-USDT",
     *     timestamp
     * );
     *
     * if (snapshot) {
     *     // Use snapshot
     *     pool.deallocate(snapshot);
     * } else {
     *     // Pool exhausted - handle error
     * }
     * @endcode
     *
     * @note Thread-safe for concurrent allocations
     * @note Uses linear probing - may retry up to POOL_SIZE times
     * @note Returns immediately if pool exhausted (no blocking)
     * @see deallocate()
     */
    template<typename... Args>
    T* allocate(Args&&... args) {
        // Try to find a free slot
        size_t attempts = 0;
        size_t start_idx = head_.fetch_add(1, std::memory_order_relaxed) % POOL_SIZE;

        while (attempts < POOL_SIZE) {
            size_t idx = (start_idx + attempts) % POOL_SIZE;

            bool expected = false;
            if (in_use_[idx].compare_exchange_strong(
                    expected, true,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {

                // Construct object in-place
                T* ptr = reinterpret_cast<T*>(&storage_[idx]);
                new (ptr) T(std::forward<Args>(args)...);
                return ptr;
            }

            ++attempts;
        }

        // Pool exhausted, return nullptr
        return nullptr;
    }

    /**
     * @brief Deallocate and destroy object
     *
     * Calls destructor on object and marks slot as available.
     * Validates that pointer is within pool bounds before deallocation.
     *
     * @param ptr Pointer to object (must be from this pool, or nullptr)
     *
     * @code
     * LockFreePool<PriceLevel, 1000> pool;
     * PriceLevel* level = pool.allocate(price, qty, ts);
     *
     * // Use object...
     *
     * pool.deallocate(level);  // Safe to call
     * pool.deallocate(nullptr); // Also safe (no-op)
     * @endcode
     *
     * @note Thread-safe for concurrent deallocations
     * @note Safe to call with nullptr (no-op)
     * @note Validates pointer is from this pool (silently ignores invalid)
     * @warning Do not use object after deallocation (use-after-free)
     * @see allocate()
     */
    void deallocate(T* ptr) {
        if (ptr == nullptr) {
            return;
        }

        // Verify pointer is within pool bounds
        uintptr_t pool_start = reinterpret_cast<uintptr_t>(&storage_[0]);
        uintptr_t pool_end = reinterpret_cast<uintptr_t>(&storage_[POOL_SIZE]);
        uintptr_t ptr_addr = reinterpret_cast<uintptr_t>(ptr);

        if (ptr_addr < pool_start || ptr_addr >= pool_end) {
            return; // Invalid pointer
        }

        // Calculate index
        size_t idx = (ptr_addr - pool_start) / sizeof(AlignedStorage);

        if (idx >= POOL_SIZE) {
            return; // Invalid index
        }

        // Destroy object
        ptr->~T();

        // Mark as free
        in_use_[idx].store(false, std::memory_order_release);
    }

    /**
     * @brief Get pool capacity
     *
     * Returns the maximum number of objects that can be allocated
     * simultaneously.
     *
     * @return constexpr size_t Pool capacity (POOL_SIZE)
     *
     * @note Compile-time constant
     */
    constexpr size_t capacity() const {
        return POOL_SIZE;
    }

    /**
     * @brief Get number of allocated objects
     *
     * Returns approximate count of currently allocated objects.
     * Result may be stale in highly concurrent scenarios.
     *
     * @return size_t Number of objects currently allocated
     *
     * @code
     * LockFreePool<PriceLevel, 1000> pool;
     *
     * // Monitor pool usage
     * if (pool.allocated() > pool.capacity() * 0.9) {
     *     std::cerr << "Warning: Pool 90% full!\n";
     * }
     * @endcode
     *
     * @note Not atomic with allocate/deallocate operations
     * @note Expensive operation - O(POOL_SIZE) - use sparingly
     * @see available()
     */
    size_t allocated() const {
        size_t count = 0;
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            if (in_use_[i].load(std::memory_order_acquire)) {
                ++count;
            }
        }
        return count;
    }

    /**
     * @brief Get number of available slots
     *
     * Returns approximate count of free slots available for allocation.
     *
     * @return size_t Number of available slots
     *
     * @note Not atomic with allocate/deallocate operations
     * @note Expensive operation - O(POOL_SIZE) - use sparingly
     * @see allocated()
     */
    size_t available() const {
        return POOL_SIZE - allocated();
    }

private:
    /**
     * @brief Aligned storage for objects
     *
     * Provides properly aligned uninitialized storage for objects.
     * Objects are constructed in-place using placement new.
     */
    struct alignas(alignof(T)) AlignedStorage {
        unsigned char data[sizeof(T)];  ///< Raw storage
    };

    alignas(64) std::atomic<size_t> head_;                      ///< Next allocation hint (cache-line aligned)
    alignas(64) std::array<std::atomic<bool>, POOL_SIZE> in_use_;  ///< Slot availability (cache-line aligned)
    std::array<AlignedStorage, POOL_SIZE> storage_;             ///< Object storage array
};

/**
 * @brief RAII wrapper for pool-allocated objects
 *
 * Automatically deallocates pool object when going out of scope,
 * similar to std::unique_ptr but for pool-allocated objects.
 *
 * **Ownership:**
 * - Move-only (no copying)
 * - Automatically deallocates on destruction
 * - Can release ownership with release()
 *
 * @tparam T Object type
 * @tparam POOL_SIZE Pool size (must match pool)
 *
 * @code
 * LockFreePool<OrderBookSnapshot, 1000> pool;
 *
 * {
 *     // Create RAII wrapper
 *     PoolPtr<OrderBookSnapshot, 1000> snapshot_ptr(
 *         pool,
 *         pool.allocate(Exchange::BINANCE, "BTC-USDT", timestamp)
 *     );
 *
 *     if (snapshot_ptr) {
 *         snapshot_ptr->bid_count = 100;
 *         process_snapshot(*snapshot_ptr);
 *     }
 * }  // Automatically deallocated here
 *
 * // Move semantics
 * auto ptr1 = PoolPtr(pool, pool.allocate(args...));
 * auto ptr2 = std::move(ptr1);  // Ownership transferred
 * // ptr1 is now nullptr
 * @endcode
 *
 * @note Move-only (cannot be copied)
 * @note Behaves like std::unique_ptr for pool objects
 * @see LockFreePool
 */
template<typename T, size_t POOL_SIZE>
class PoolPtr {
public:
    /**
     * @brief Construct PoolPtr from pool and pointer
     *
     * Takes ownership of pool-allocated object. Will deallocate
     * on destruction unless ownership is released.
     *
     * @param pool Reference to pool that owns the storage
     * @param ptr Pointer to pool-allocated object (may be nullptr)
     *
     * @code
     * LockFreePool<int, 100> pool;
     * PoolPtr<int, 100> ptr(pool, pool.allocate(42));
     * @endcode
     */
    PoolPtr(LockFreePool<T, POOL_SIZE>& pool, T* ptr)
        : pool_(pool)
        , ptr_(ptr) {}

    /**
     * @brief Destroy PoolPtr and deallocate object
     *
     * Automatically deallocates object if still owned.
     */
    ~PoolPtr() {
        if (ptr_) {
            pool_.deallocate(ptr_);
        }
    }

    // Move-only
    PoolPtr(const PoolPtr&) = delete;
    PoolPtr& operator=(const PoolPtr&) = delete;

    /**
     * @brief Move constructor
     *
     * Transfers ownership from other PoolPtr.
     *
     * @param other PoolPtr to move from (becomes nullptr)
     */
    PoolPtr(PoolPtr&& other) noexcept
        : pool_(other.pool_)
        , ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    /**
     * @brief Move assignment operator
     *
     * Deallocates currently owned object (if any) and takes
     * ownership from other PoolPtr.
     *
     * @param other PoolPtr to move from (becomes nullptr)
     * @return PoolPtr& Reference to this
     */
    PoolPtr& operator=(PoolPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                pool_.deallocate(ptr_);
            }
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Arrow operator for member access
     *
     * @return T* Pointer to owned object
     */
    T* operator->() { return ptr_; }

    /**
     * @brief Arrow operator for member access (const)
     *
     * @return const T* Pointer to owned object
     */
    const T* operator->() const { return ptr_; }

    /**
     * @brief Dereference operator
     *
     * @return T& Reference to owned object
     */
    T& operator*() { return *ptr_; }

    /**
     * @brief Dereference operator (const)
     *
     * @return const T& Reference to owned object
     */
    const T& operator*() const { return *ptr_; }

    /**
     * @brief Get raw pointer
     *
     * @return T* Pointer to owned object (nullptr if none)
     */
    T* get() { return ptr_; }

    /**
     * @brief Get raw pointer (const)
     *
     * @return const T* Pointer to owned object (nullptr if none)
     */
    const T* get() const { return ptr_; }

    /**
     * @brief Check if pointer is valid
     *
     * @return bool True if owns an object, false if nullptr
     *
     * @code
     * PoolPtr<int, 100> ptr(pool, pool.allocate(42));
     * if (ptr) {
     *     // Safe to use
     * }
     * @endcode
     */
    explicit operator bool() const { return ptr_ != nullptr; }

    /**
     * @brief Release ownership without deallocation
     *
     * Returns the owned pointer and sets internal pointer to nullptr.
     * Caller becomes responsible for deallocation.
     *
     * @return T* Pointer to previously owned object
     *
     * @code
     * PoolPtr<int, 100> ptr(pool, pool.allocate(42));
     * int* raw = ptr.release();  // ptr is now nullptr
     * // Must manually deallocate:
     * pool.deallocate(raw);
     * @endcode
     *
     * @warning Caller must manually deallocate returned pointer
     */
    T* release() {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

private:
    LockFreePool<T, POOL_SIZE>& pool_;  ///< Reference to pool
    T* ptr_;                             ///< Owned pointer
};

} // namespace marketdata
