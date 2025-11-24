/**
 * @file cpu_affinity.hpp
 * @brief CPU affinity and thread pinning utilities
 *
 * Provides low-level CPU affinity control for performance-critical threads.
 * Enables pinning threads to specific CPU cores to reduce context switching,
 * improve cache locality, and achieve more consistent latencies.
 *
 * **Features:**
 * - Thread-to-core pinning
 * - CPU core enumeration
 * - Real-time scheduling priority
 * - CPU isolation validation
 * - Cross-platform support (Linux/Windows)
 *
 * **Use Cases:**
 * - Pin order book processing to dedicated cores
 * - Pin network I/O threads to specific cores
 * - Isolate critical threads from OS scheduler
 * - Reduce cache thrashing between threads
 *
 * **Performance Impact:**
 * - 10-30% latency reduction for critical paths
 * - More consistent P99/P999 latencies
 * - Better L1/L2 cache hit rates
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace marketdata {

/**
 * @brief Thread scheduling policy
 */
enum class SchedulingPolicy {
    NORMAL,         ///< Default scheduling
    REALTIME_FIFO,  ///< Real-time FIFO (requires root on Linux)
    REALTIME_RR,    ///< Real-time Round-Robin (requires root on Linux)
    BATCH,          ///< Batch processing (low priority)
    IDLE            ///< Idle (very low priority)
};

/**
 * @brief CPU core information
 */
struct CPUCoreInfo {
    uint32_t core_id;           ///< Physical core ID
    uint32_t numa_node;         ///< NUMA node ID
    bool is_isolated;           ///< Whether core is isolated from scheduler
    bool is_online;             ///< Whether core is online
    uint64_t frequency_mhz;     ///< Current frequency in MHz
};

/**
 * @brief Thread affinity mask
 *
 * Represents a set of CPU cores that a thread can run on.
 */
class CPUAffinityMask {
public:
    /**
     * @brief Construct empty affinity mask
     */
    CPUAffinityMask() = default;

    /**
     * @brief Construct affinity mask from core list
     *
     * @param cores List of core IDs
     */
    explicit CPUAffinityMask(const std::vector<uint32_t>& cores);

    /**
     * @brief Add core to affinity mask
     *
     * @param core_id Core ID to add
     */
    void add_core(uint32_t core_id);

    /**
     * @brief Remove core from affinity mask
     *
     * @param core_id Core ID to remove
     */
    void remove_core(uint32_t core_id);

    /**
     * @brief Check if core is in affinity mask
     *
     * @param core_id Core ID to check
     * @return bool True if core is in mask
     */
    bool has_core(uint32_t core_id) const;

    /**
     * @brief Get all cores in mask
     *
     * @return std::vector<uint32_t> List of core IDs
     */
    std::vector<uint32_t> get_cores() const;

    /**
     * @brief Clear all cores from mask
     */
    void clear();

    /**
     * @brief Check if mask is empty
     *
     * @return bool True if no cores in mask
     */
    bool empty() const { return cores_.empty(); }

    /**
     * @brief Get number of cores in mask
     *
     * @return size_t Number of cores
     */
    size_t size() const { return cores_.size(); }

private:
    std::vector<uint32_t> cores_;   ///< Core IDs in mask
};

/**
 * @brief CPU affinity utilities
 *
 * Static utility class for CPU affinity and thread pinning operations.
 *
 * **Thread Pinning Example:**
 * @code
 * // Pin current thread to core 2
 * if (CPUAffinity::set_thread_affinity(2)) {
 *     spdlog::info("Thread pinned to core 2");
 * }
 *
 * // Pin specific thread to multiple cores
 * CPUAffinityMask mask({2, 3, 4});
 * CPUAffinity::set_thread_affinity(thread_handle, mask);
 * @endcode
 *
 * **Real-time Priority:**
 * @code
 * // Set real-time FIFO priority (requires root on Linux)
 * if (CPUAffinity::set_thread_priority(SchedulingPolicy::REALTIME_FIFO, 99)) {
 *     spdlog::info("Thread running with RT priority");
 * }
 * @endcode
 *
 * **CPU Enumeration:**
 * @code
 * // Get all available cores
 * auto cores = CPUAffinity::get_available_cores();
 * spdlog::info("System has {} cores", cores.size());
 *
 * // Get isolated cores (from kernel cmdline isolcpus=)
 * auto isolated = CPUAffinity::get_isolated_cores();
 * for (auto& core : isolated) {
 *     spdlog::info("Core {} is isolated", core.core_id);
 * }
 * @endcode
 *
 * @note Thread pinning may require elevated privileges
 * @note Real-time scheduling requires root on Linux
 * @note Not all platforms support all features
 */
class CPUAffinity {
public:
    // Disable construction - static utility class
    CPUAffinity() = delete;

    /**
     * @brief Get number of available CPU cores
     *
     * @return uint32_t Number of cores
     *
     * @note Returns logical cores (includes hyperthreading)
     */
    static uint32_t get_core_count();

    /**
     * @brief Get information about available cores
     *
     * @return std::vector<CPUCoreInfo> List of core information
     *
     * @note May require elevated privileges for full information
     */
    static std::vector<CPUCoreInfo> get_available_cores();

    /**
     * @brief Get isolated cores
     *
     * Returns cores isolated from the Linux scheduler via kernel boot
     * parameter isolcpus=. These cores are ideal for pinning latency-
     * sensitive threads.
     *
     * @return std::vector<CPUCoreInfo> List of isolated cores
     *
     * @note Linux only - returns empty on other platforms
     * @note Requires isolcpus= kernel parameter
     */
    static std::vector<CPUCoreInfo> get_isolated_cores();

    /**
     * @brief Pin current thread to specific core
     *
     * @param core_id CPU core ID (0-based)
     * @return bool True if successful
     *
     * @code
     * // Pin network thread to core 2
     * if (CPUAffinity::set_thread_affinity(2)) {
     *     spdlog::info("Network thread pinned to core 2");
     * }
     * @endcode
     */
    static bool set_thread_affinity(uint32_t core_id);

    /**
     * @brief Pin current thread to multiple cores
     *
     * @param mask Affinity mask with core IDs
     * @return bool True if successful
     *
     * @code
     * // Pin thread to cores 2, 3, 4
     * CPUAffinityMask mask({2, 3, 4});
     * CPUAffinity::set_thread_affinity(mask);
     * @endcode
     */
    static bool set_thread_affinity(const CPUAffinityMask& mask);

    /**
     * @brief Pin specific thread to core
     *
     * @param thread Thread to pin
     * @param core_id CPU core ID
     * @return bool True if successful
     */
    static bool set_thread_affinity(std::thread& thread, uint32_t core_id);

    /**
     * @brief Pin specific thread to multiple cores
     *
     * @param thread Thread to pin
     * @param mask Affinity mask
     * @return bool True if successful
     */
    static bool set_thread_affinity(std::thread& thread, const CPUAffinityMask& mask);

    /**
     * @brief Get current thread's affinity mask
     *
     * @return std::optional<CPUAffinityMask> Affinity mask, or nullopt on error
     */
    static std::optional<CPUAffinityMask> get_thread_affinity();

    /**
     * @brief Get specific thread's affinity mask
     *
     * @param thread Thread to query
     * @return std::optional<CPUAffinityMask> Affinity mask, or nullopt on error
     */
    static std::optional<CPUAffinityMask> get_thread_affinity(std::thread& thread);

    /**
     * @brief Set scheduling policy and priority for current thread
     *
     * @param policy Scheduling policy
     * @param priority Priority level (1-99 for real-time, 0 for normal)
     * @return bool True if successful
     *
     * @note Real-time policies require root on Linux
     * @note Priority 99 is highest, 1 is lowest for real-time
     *
     * @code
     * // Set real-time FIFO with highest priority
     * if (CPUAffinity::set_thread_priority(SchedulingPolicy::REALTIME_FIFO, 99)) {
     *     spdlog::info("Thread running with RT priority");
     * } else {
     *     spdlog::warn("Failed to set RT priority - requires root");
     * }
     * @endcode
     */
    static bool set_thread_priority(SchedulingPolicy policy, int priority = 0);

    /**
     * @brief Set scheduling policy and priority for specific thread
     *
     * @param thread Thread to configure
     * @param policy Scheduling policy
     * @param priority Priority level
     * @return bool True if successful
     */
    static bool set_thread_priority(std::thread& thread, SchedulingPolicy policy, int priority = 0);

    /**
     * @brief Check if current thread is running on isolated core
     *
     * @return bool True if on isolated core
     *
     * @note Linux only - always returns false on other platforms
     */
    static bool is_on_isolated_core();

    /**
     * @brief Get current CPU core ID
     *
     * Returns the CPU core that the current thread is currently
     * executing on. Note that this can change if the thread is not
     * pinned.
     *
     * @return std::optional<uint32_t> Core ID, or nullopt on error
     */
    static std::optional<uint32_t> get_current_core();

    /**
     * @brief Validate CPU configuration for trading
     *
     * Checks if the system is properly configured for low-latency trading:
     * - Isolated cores available
     * - CPU frequency scaling disabled
     * - Real-time scheduling available
     * - NUMA configuration optimal
     *
     * @return std::vector<std::string> List of warnings/issues (empty if all good)
     *
     * @code
     * auto warnings = CPUAffinity::validate_configuration();
     * if (warnings.empty()) {
     *     spdlog::info("CPU configuration optimal for trading");
     * } else {
     *     for (const auto& warning : warnings) {
     *         spdlog::warn("CPU config: {}", warning);
     *     }
     * }
     * @endcode
     */
    static std::vector<std::string> validate_configuration();

private:
    /**
     * @brief Parse /proc/cmdline for isolcpus (Linux)
     */
    static std::vector<uint32_t> parse_isolcpus();

    /**
     * @brief Check if CPU frequency scaling is disabled
     */
    static bool is_frequency_scaling_disabled();

    /**
     * @brief Get CPU governor (Linux)
     */
    static std::string get_cpu_governor();
};

/**
 * @brief RAII helper for thread affinity
 *
 * Automatically restores original thread affinity when destroyed.
 * Useful for temporarily pinning threads in specific code sections.
 *
 * @code
 * {
 *     // Pin to core 2 for this scope
 *     ThreadAffinityGuard guard(2);
 *
 *     // Critical low-latency code here
 *     process_order_book_update();
 *
 * } // Automatically restored to original affinity
 * @endcode
 */
class ThreadAffinityGuard {
public:
    /**
     * @brief Pin current thread to core and save original affinity
     *
     * @param core_id Core to pin to
     */
    explicit ThreadAffinityGuard(uint32_t core_id);

    /**
     * @brief Pin current thread to multiple cores and save original affinity
     *
     * @param mask Affinity mask
     */
    explicit ThreadAffinityGuard(const CPUAffinityMask& mask);

    /**
     * @brief Restore original affinity
     */
    ~ThreadAffinityGuard();

    // Disable copy and move
    ThreadAffinityGuard(const ThreadAffinityGuard&) = delete;
    ThreadAffinityGuard& operator=(const ThreadAffinityGuard&) = delete;

private:
    std::optional<CPUAffinityMask> original_affinity_;
    bool restore_on_destroy_{true};
};

} // namespace marketdata
