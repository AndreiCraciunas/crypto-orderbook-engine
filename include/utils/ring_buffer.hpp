/**
 * @file ring_buffer.hpp
 * @brief Lock-free single-producer single-consumer ring buffer
 *
 * Provides a high-performance circular buffer for passing data between
 * exactly one producer and one consumer thread. Uses power-of-2 sizing
 * and atomic operations for lock-free operation.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <cstddef>

namespace marketdata {

/**
 * @brief Lock-free single-producer single-consumer ring buffer
 *
 * A circular buffer optimized for passing data between one producer
 * thread and one consumer thread without locks. Uses atomic operations
 * with relaxed memory ordering for maximum performance.
 *
 * **Algorithm:**
 * - Head index: where producer writes next
 * - Tail index: where consumer reads next
 * - Empty when head == tail
 * - Full when (head + 1) % SIZE == tail
 * - Uses power-of-2 size for fast modulo (bit mask)
 *
 * **Performance:**
 * - Push: O(1), ~10-20 ns typical
 * - Pop: O(1), ~10-20 ns typical
 * - Zero contention between threads
 * - No CAS loops (unlike MPMC queues)
 *
 * @tparam T Element type (must be movable/copyable)
 * @tparam CAPACITY Buffer capacity (must be power of 2)
 *
 * @code
 * RingBuffer<int, 1024> buffer;
 *
 * // Producer thread
 * if (buffer.push(42)) {
 *     // Success
 * }
 *
 * // Consumer thread
 * auto value = buffer.pop();
 * if (value) {
 *     std::cout << *value << '\n';
 * }
 * @endcode
 *
 * @note Thread-safe for one producer and one consumer only
 * @warning CAPACITY must be a power of 2
 * @warning Not safe for multiple producers or consumers
 * @see LockFreeQueue for MPMC scenarios
 */
template<typename T, size_t CAPACITY>
class RingBuffer {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of 2");

public:
    /**
     * @brief Construct empty ring buffer
     *
     * Initializes head and tail to 0.
     */
    RingBuffer()
        : head_(0)
        , tail_(0) {}

    /**
     * @brief Push element to buffer (copy)
     *
     * Attempts to add an element to the buffer. Fails if buffer is full.
     *
     * @param value Element to push
     * @return true if pushed successfully, false if buffer was full
     *
     * @code
     * RingBuffer<int, 16> buffer;
     * if (buffer.push(42)) {
     *     std::cout << "Pushed successfully\n";
     * } else {
     *     std::cout << "Buffer full\n";
     * }
     * @endcode
     *
     * @note Thread-safe only if called from single producer thread
     * @see push(T&&) for move version
     */
    bool push(const T& value) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (head + 1) & MASK;

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // Buffer is full
        }

        buffer_[head] = value;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /**
     * @brief Push element to buffer (move)
     *
     * Move version of push() for efficiency with move-only types.
     *
     * @param value Element to push (will be moved)
     * @return true if pushed successfully, false if buffer was full
     *
     * @code
     * RingBuffer<std::unique_ptr<int>, 16> buffer;
     * buffer.push(std::make_unique<int>(42));  // Move
     * @endcode
     *
     * @note Thread-safe only if called from single producer thread
     * @see push(const T&)
     */
    bool push(T&& value) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (head + 1) & MASK;

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // Buffer is full
        }

        buffer_[head] = std::move(value);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /**
     * @brief Pop element from buffer
     *
     * Attempts to remove and return an element from the buffer.
     * Returns std::nullopt if buffer is empty.
     *
     * @return std::optional<T> Element if available, std::nullopt if empty
     *
     * @code
     * RingBuffer<int, 16> buffer;
     * buffer.push(42);
     *
     * auto value = buffer.pop();
     * if (value) {
     *     std::cout << "Got: " << *value << '\n';
     * } else {
     *     std::cout << "Buffer empty\n";
     * }
     * @endcode
     *
     * @note Thread-safe only if called from single consumer thread
     */
    std::optional<T> pop() {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt; // Buffer is empty
        }

        T value = std::move(buffer_[tail]);
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return value;
    }

    /**
     * @brief Peek at front element without removing
     *
     * Returns a copy of the front element without removing it.
     *
     * @return std::optional<T> Front element if available, std::nullopt if empty
     *
     * @code
     * auto value = buffer.peek();
     * if (value) {
     *     std::cout << "Front: " << *value << '\n';
     * }
     * // Element still in buffer
     * @endcode
     *
     * @note Thread-safe for consumer thread only
     */
    std::optional<T> peek() const {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt; // Buffer is empty
        }

        return buffer_[tail];
    }

    /**
     * @brief Check if buffer is empty
     *
     * @return true if empty, false otherwise
     *
     * @note Result may be stale in multi-threaded context
     */
    bool empty() const {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    /**
     * @brief Check if buffer is full
     *
     * @return true if full, false otherwise
     *
     * @note Result may be stale in multi-threaded context
     */
    bool full() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t next_head = (head + 1) & MASK;
        return next_head == tail_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get current number of elements
     *
     * Returns approximate size (may be stale in concurrent scenarios).
     *
     * @return size_t Number of elements currently in buffer
     *
     * @note Not atomic with other operations
     */
    size_t size() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & MASK;
    }

    /**
     * @brief Get buffer capacity
     *
     * @return constexpr size_t Maximum number of elements
     */
    constexpr size_t capacity() const {
        return CAPACITY;
    }

    /**
     * @brief Clear all elements from buffer
     *
     * Resets head and tail to 0. Not thread-safe - call only
     * when buffer is not in use by other threads.
     *
     * @warning Not thread-safe - ensure exclusive access
     */
    void clear() {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }

private:
    static constexpr size_t MASK = CAPACITY - 1;  ///< Bit mask for fast modulo

    alignas(64) std::atomic<size_t> head_;  ///< Producer write position (cache-line aligned)
    alignas(64) std::atomic<size_t> tail_;  ///< Consumer read position (cache-line aligned)
    std::array<T, CAPACITY> buffer_;        ///< Storage array
};

} // namespace marketdata
