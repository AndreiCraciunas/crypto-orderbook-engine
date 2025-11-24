/**
 * @file seq_lock.hpp
 * @brief Sequence lock implementation for lock-free consistent reads
 *
 * Provides a SeqLock (sequence lock) implementation for scenarios where
 * writes are rare but need to be fast, and reads are frequent but can
 * tolerate retries. Used extensively for order book snapshots.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace marketdata {

/**
 * @brief Sequence lock for lock-free consistent reads
 *
 * SeqLock provides optimistic concurrency control where readers check
 * a sequence number before and after reading data. If the sequence
 * changed (indicating a concurrent write), the read is retried.
 *
 * **Algorithm:**
 * - Sequence starts at 0 (even = stable)
 * - Writers increment before write (odd = writing)
 * - Writers increment after write (even = stable)
 * - Readers retry if sequence is odd or changed
 *
 * **Performance:**
 * - Write overhead: ~10-20 ns (two atomic increments)
 * - Read overhead: ~5-10 ns when no contention
 * - Readers never block writers
 * - Multiple concurrent readers supported
 *
 * @code
 * SeqLock lock;
 * int data = 0;
 *
 * // Writer
 * {
 *     SeqLock::WriteGuard guard(lock);
 *     data = 42;  // Protected write
 * }
 *
 * // Reader
 * int value;
 * SeqLock::ReadGuard guard(lock);
 * do {
 *     guard.begin();
 *     value = data;  // May be inconsistent
 * } while (guard.retry());  // Retry if data changed
 * @endcode
 *
 * @note Write-preferring: writers never wait
 * @note Readers may starve under heavy write load
 * @warning Protected data must be copyable
 */
class SeqLock {
public:
    /**
     * @brief Construct a new SeqLock
     *
     * Initializes sequence number to 0 (stable state).
     */
    SeqLock() : sequence_(0) {}

    /**
     * @brief Acquire write lock
     *
     * Increments sequence to odd number, indicating write in progress.
     * Must be paired with write_unlock().
     *
     * @note Use WriteGuard RAII wrapper instead
     * @see WriteGuard
     */
    void write_lock() {
        sequence_.fetch_add(1, std::memory_order_acquire);
    }

    /**
     * @brief Release write lock
     *
     * Increments sequence to even number, indicating stable state.
     * Must be called after write_lock().
     *
     * @note Use WriteGuard RAII wrapper instead
     * @see WriteGuard
     */
    void write_unlock() {
        sequence_.fetch_add(1, std::memory_order_release);
    }

    /**
     * @brief Begin optimistic read
     *
     * Returns current sequence number for later validation.
     * Spins if sequence is odd (writer active).
     *
     * @return uint64_t Sequence number when read began
     *
     * @note Use ReadGuard RAII wrapper instead
     * @see read_retry()
     * @see ReadGuard
     */
    uint64_t read_begin() const {
        uint64_t seq;
        do {
            seq = sequence_.load(std::memory_order_acquire);
        } while (seq & 1); // Wait if odd (writer active)
        return seq;
    }

    /**
     * @brief Check if read needs retry
     *
     * Compares current sequence with saved value. Returns true
     * if sequence changed, indicating data may be inconsistent.
     *
     * @param seq Sequence number from read_begin()
     * @return bool True if retry needed, false if read was consistent
     *
     * @note Use ReadGuard RAII wrapper instead
     * @see read_begin()
     * @see ReadGuard
     */
    bool read_retry(uint64_t seq) const {
        std::atomic_thread_fence(std::memory_order_acquire);
        return sequence_.load(std::memory_order_acquire) != seq;
    }

    /**
     * @brief RAII write guard
     *
     * Automatically acquires write lock on construction and
     * releases on destruction. Prevents forgetting to unlock.
     *
     * @code
     * SeqLock lock;
     * int data = 0;
     *
     * {
     *     SeqLock::WriteGuard guard(lock);
     *     data = 42;  // Automatically locked
     * }  // Automatically unlocked
     * @endcode
     *
     * @note Non-copyable and non-movable
     */
    class WriteGuard {
    public:
        /**
         * @brief Construct guard and acquire write lock
         *
         * @param lock SeqLock to protect
         */
        explicit WriteGuard(SeqLock& lock) : lock_(lock) {
            lock_.write_lock();
        }

        /**
         * @brief Destroy guard and release write lock
         */
        ~WriteGuard() {
            lock_.write_unlock();
        }

        // Non-copyable, non-movable
        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;
        WriteGuard(WriteGuard&&) = delete;
        WriteGuard& operator=(WriteGuard&&) = delete;

    private:
        SeqLock& lock_;  ///< Protected lock
    };

    /**
     * @brief RAII read guard with retry loop
     *
     * Simplifies the read-retry pattern by providing begin() and
     * retry() methods that manage sequence numbers automatically.
     *
     * @code
     * SeqLock lock;
     * int data = 0;
     *
     * SeqLock::ReadGuard guard(lock);
     * int value;
     * do {
     *     guard.begin();
     *     value = data;
     * } while (guard.retry());
     * // value is now consistent
     * @endcode
     */
    class ReadGuard {
    public:
        /**
         * @brief Construct read guard
         *
         * @param lock SeqLock to read from
         */
        explicit ReadGuard(const SeqLock& lock)
            : lock_(lock)
            , seq_(0)
            , valid_(false) {}

        /**
         * @brief Begin read operation
         *
         * Captures current sequence number. Call before reading data.
         *
         * @return uint64_t Sequence number
         */
        uint64_t begin() {
            seq_ = lock_.read_begin();
            valid_ = false;
            return seq_;
        }

        /**
         * @brief Check if retry needed
         *
         * Returns true if sequence changed (retry needed),
         * false if read was consistent.
         *
         * @return bool True to retry, false if done
         */
        bool retry() {
            if (lock_.read_retry(seq_)) {
                return true; // Need to retry
            }
            valid_ = true;
            return false; // Read was consistent
        }

        /**
         * @brief Check if last read was valid
         *
         * @return bool True if retry() returned false at least once
         */
        bool is_valid() const {
            return valid_;
        }

    private:
        const SeqLock& lock_;  ///< Lock being read
        uint64_t seq_;         ///< Saved sequence number
        bool valid_;           ///< Whether read succeeded
    };

private:
    alignas(64) std::atomic<uint64_t> sequence_;  ///< Sequence counter (cache-line aligned)
};

} // namespace marketdata
