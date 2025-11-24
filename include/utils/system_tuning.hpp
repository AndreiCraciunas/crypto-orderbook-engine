/**
 * @file system_tuning.hpp
 * @brief System tuning utilities for production deployment
 *
 * Provides low-level system configuration for ultra-low latency trading:
 * - NUMA-aware memory allocation
 * - Huge pages (2MB/1GB) support
 * - Memory locking (mlockall)
 * - Transparent huge pages control
 * - Network stack tuning
 * - Kernel bypass preparation
 *
 * **Production Hardening:**
 * - Reduce page faults with huge pages
 * - Lock memory to prevent swapping
 * - NUMA-local allocations
 * - Disable transparent huge pages
 * - Tune network buffers
 *
 * **Latency Impact:**
 * - Huge pages: 10-30% reduction in TLB misses
 * - Memory locking: Eliminates swap-related latency spikes
 * - NUMA locality: 20-50% faster memory access
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <optional>

#ifdef __linux__
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#endif

namespace marketdata {

/**
 * @brief Huge page size
 */
enum class HugePageSize {
    NORMAL_4KB,     ///< Normal 4KB pages (default)
    HUGE_2MB,       ///< 2MB huge pages (hugepages)
    HUGE_1GB        ///< 1GB huge pages (gigantic pages)
};

/**
 * @brief NUMA node information
 */
struct NUMANodeInfo {
    uint32_t node_id;               ///< NUMA node ID
    uint64_t total_memory_mb;       ///< Total memory in MB
    uint64_t free_memory_mb;        ///< Free memory in MB
    std::vector<uint32_t> cpu_cores; ///< CPU cores on this node
    bool is_local;                  ///< Is this the current thread's node?
};

/**
 * @brief Memory allocation statistics
 */
struct MemoryStats {
    uint64_t allocated_bytes;       ///< Total allocated bytes
    uint64_t huge_pages_allocated;  ///< Number of huge pages allocated
    uint64_t numa_node;             ///< NUMA node of allocation
    bool is_locked;                 ///< Is memory locked (mlockall)?
    bool uses_huge_pages;           ///< Uses huge pages?
};

/**
 * @brief NUMA-aware allocator
 *
 * Custom allocator that ensures memory is allocated on specific NUMA nodes
 * for optimal cache locality and memory bandwidth.
 *
 * @tparam T Type to allocate
 *
 * @code
 * // Allocate on NUMA node 0
 * NUMAAllocator<int> allocator(0);
 * std::vector<int, NUMAAllocator<int>> vec(allocator);
 * vec.reserve(1000000);
 * @endcode
 */
template<typename T>
class NUMAAllocator {
public:
    using value_type = T;

    /**
     * @brief Construct allocator for specific NUMA node
     *
     * @param numa_node NUMA node to allocate on
     */
    explicit NUMAAllocator(uint32_t numa_node = 0) noexcept
        : numa_node_(numa_node) {}

    /**
     * @brief Copy constructor
     */
    template<typename U>
    NUMAAllocator(const NUMAAllocator<U>& other) noexcept
        : numa_node_(other.numa_node_) {}

    /**
     * @brief Allocate memory on NUMA node
     *
     * @param n Number of elements
     * @return T* Pointer to allocated memory
     */
    T* allocate(std::size_t n);

    /**
     * @brief Deallocate memory
     *
     * @param p Pointer to deallocate
     * @param n Number of elements
     */
    void deallocate(T* p, std::size_t n) noexcept;

    /**
     * @brief Get NUMA node
     */
    uint32_t get_numa_node() const { return numa_node_; }

    template<typename U>
    friend class NUMAAllocator;

private:
    uint32_t numa_node_;
};

template<typename T, typename U>
bool operator==(const NUMAAllocator<T>& lhs, const NUMAAllocator<U>& rhs) {
    return lhs.get_numa_node() == rhs.get_numa_node();
}

template<typename T, typename U>
bool operator!=(const NUMAAllocator<T>& lhs, const NUMAAllocator<U>& rhs) {
    return !(lhs == rhs);
}

/**
 * @brief System tuning utilities
 *
 * Static utility class for system-level performance tuning.
 *
 * **Huge Pages Setup:**
 * @code
 * // Check huge page availability
 * auto info = SystemTuning::get_huge_page_info();
 * spdlog::info("Huge pages available: {}", info.available_2mb);
 *
 * // Enable huge pages for process
 * if (SystemTuning::enable_huge_pages(HugePageSize::HUGE_2MB)) {
 *     spdlog::info("Huge pages enabled");
 * }
 * @endcode
 *
 * **Memory Locking:**
 * @code
 * // Lock all memory to prevent swapping
 * if (SystemTuning::lock_memory()) {
 *     spdlog::info("Memory locked - no swapping");
 * } else {
 *     spdlog::warn("Failed to lock memory - requires root or CAP_IPC_LOCK");
 * }
 * @endcode
 *
 * **NUMA:**
 * @code
 * // Get NUMA information
 * auto nodes = SystemTuning::get_numa_nodes();
 * for (const auto& node : nodes) {
 *     spdlog::info("NUMA node {}: {} MB, {} cores",
 *                  node.node_id, node.free_memory_mb, node.cpu_cores.size());
 * }
 *
 * // Allocate on local NUMA node
 * void* ptr = SystemTuning::allocate_numa(1024 * 1024, 0);  // 1MB on node 0
 * @endcode
 *
 * **System Validation:**
 * @code
 * // Validate production readiness
 * auto warnings = SystemTuning::validate_production_config();
 * if (warnings.empty()) {
 *     spdlog::info("System is production-ready");
 * } else {
 *     for (const auto& warning : warnings) {
 *         spdlog::warn("{}", warning);
 *     }
 * }
 * @endcode
 *
 * @note Many operations require root privileges
 * @note Linux-specific features may not work on other platforms
 */
class SystemTuning {
public:
    // Disable construction - static utility class
    SystemTuning() = delete;

    /**
     * @brief Huge page information
     */
    struct HugePageInfo {
        uint64_t available_2mb;     ///< Available 2MB huge pages
        uint64_t available_1gb;     ///< Available 1GB huge pages
        uint64_t total_2mb;         ///< Total configured 2MB huge pages
        uint64_t total_1gb;         ///< Total configured 1GB huge pages
        uint64_t page_size_2mb;     ///< 2MB page size in bytes
        uint64_t page_size_1gb;     ///< 1GB page size in bytes
        bool transparent_enabled;   ///< Is transparent huge pages enabled?
    };

    /**
     * @brief Get huge page information
     *
     * @return HugePageInfo Current huge page configuration
     */
    static HugePageInfo get_huge_page_info();

    /**
     * @brief Enable huge pages for this process
     *
     * Uses madvise(MADV_HUGEPAGE) to request huge pages for future allocations.
     *
     * @param size Huge page size to use
     * @return bool True if successful
     *
     * @note Linux only
     * @note Requires huge pages configured in /proc/sys/vm/nr_hugepages
     */
    static bool enable_huge_pages(HugePageSize size = HugePageSize::HUGE_2MB);

    /**
     * @brief Allocate memory using huge pages
     *
     * @param size Size in bytes
     * @param numa_node NUMA node (optional)
     * @return void* Pointer to allocated memory, or nullptr on failure
     *
     * @code
     * // Allocate 10MB with huge pages on NUMA node 0
     * void* ptr = SystemTuning::allocate_huge_pages(10 * 1024 * 1024, 0);
     * if (ptr) {
     *     // Use memory...
     *     SystemTuning::free_huge_pages(ptr, 10 * 1024 * 1024);
     * }
     * @endcode
     *
     * @note Caller must free with free_huge_pages()
     * @note Size should be multiple of huge page size for best results
     */
    static void* allocate_huge_pages(size_t size, std::optional<uint32_t> numa_node = std::nullopt);

    /**
     * @brief Free huge page memory
     *
     * @param ptr Pointer returned by allocate_huge_pages()
     * @param size Size in bytes (same as allocation)
     */
    static void free_huge_pages(void* ptr, size_t size);

    /**
     * @brief Lock all current and future memory
     *
     * Uses mlockall(MCL_CURRENT | MCL_FUTURE) to prevent memory from
     * being swapped to disk. Critical for low-latency trading.
     *
     * @return bool True if successful
     *
     * @note Requires root or CAP_IPC_LOCK capability
     * @note May fail if RLIMIT_MEMLOCK is too low
     * @note Can cause OOM if too much memory used
     */
    static bool lock_memory();

    /**
     * @brief Unlock memory
     *
     * Reverses lock_memory() call.
     *
     * @return bool True if successful
     */
    static bool unlock_memory();

    /**
     * @brief Check if memory is locked
     *
     * @return bool True if mlockall is active
     */
    static bool is_memory_locked();

    /**
     * @brief Disable transparent huge pages
     *
     * Transparent huge pages can cause latency spikes due to compaction.
     * This disables them for more predictable performance.
     *
     * @return bool True if successful
     *
     * @note Requires root privileges
     * @note Linux only
     * @note Writes to /sys/kernel/mm/transparent_hugepage/enabled
     */
    static bool disable_transparent_huge_pages();

    /**
     * @brief Get NUMA node count
     *
     * @return uint32_t Number of NUMA nodes (1 if NUMA not available)
     */
    static uint32_t get_numa_node_count();

    /**
     * @brief Get information about all NUMA nodes
     *
     * @return std::vector<NUMANodeInfo> List of NUMA nodes
     */
    static std::vector<NUMANodeInfo> get_numa_nodes();

    /**
     * @brief Get current NUMA node
     *
     * Returns the NUMA node that the current thread is running on.
     *
     * @return std::optional<uint32_t> NUMA node ID, or nullopt on error
     */
    static std::optional<uint32_t> get_current_numa_node();

    /**
     * @brief Allocate memory on specific NUMA node
     *
     * @param size Size in bytes
     * @param numa_node NUMA node ID
     * @return void* Pointer to allocated memory, or nullptr on failure
     *
     * @note Caller must free with free_numa()
     */
    static void* allocate_numa(size_t size, uint32_t numa_node);

    /**
     * @brief Free NUMA-allocated memory
     *
     * @param ptr Pointer returned by allocate_numa()
     * @param size Size in bytes (same as allocation)
     */
    static void free_numa(void* ptr, size_t size);

    /**
     * @brief Prefault memory to avoid page faults
     *
     * Touches all pages in the given memory range to ensure they're
     * allocated and mapped before critical paths execute.
     *
     * @param ptr Pointer to memory
     * @param size Size in bytes
     *
     * @code
     * void* buffer = malloc(10 * 1024 * 1024);  // 10MB
     * SystemTuning::prefault_memory(buffer, 10 * 1024 * 1024);
     * // Now buffer is guaranteed to be resident in RAM
     * @endcode
     */
    static void prefault_memory(void* ptr, size_t size);

    /**
     * @brief Get memory statistics
     *
     * @return MemoryStats Current memory statistics
     */
    static MemoryStats get_memory_stats();

    /**
     * @brief Set network buffer sizes
     *
     * Increases socket buffer sizes for better throughput.
     *
     * @param rmem_max Max receive buffer (bytes)
     * @param wmem_max Max send buffer (bytes)
     * @return bool True if successful
     *
     * @note Requires root privileges
     * @note Linux only
     * @note Writes to /proc/sys/net/core/rmem_max and wmem_max
     */
    static bool set_network_buffers(uint64_t rmem_max, uint64_t wmem_max);

    /**
     * @brief Disable address space layout randomization
     *
     * Makes memory addresses more predictable. Useful for debugging
     * and profiling, but reduces security.
     *
     * @return bool True if successful
     *
     * @note Requires root privileges
     * @note Security impact - use only in controlled environments
     */
    static bool disable_aslr();

    /**
     * @brief Validate production configuration
     *
     * Checks system configuration for production trading:
     * - Huge pages configured
     * - Transparent huge pages disabled
     * - Sufficient memory locked
     * - NUMA configuration optimal
     * - Swap disabled
     * - Network buffers sized correctly
     *
     * @return std::vector<std::string> List of warnings/issues (empty if all good)
     *
     * @code
     * auto warnings = SystemTuning::validate_production_config();
     * if (!warnings.empty()) {
     *     spdlog::error("System not ready for production:");
     *     for (const auto& warning : warnings) {
     *         spdlog::error("  - {}", warning);
     *     }
     *     return false;
     * }
     * @endcode
     */
    static std::vector<std::string> validate_production_config();

    /**
     * @brief Apply recommended production settings
     *
     * Applies all recommended settings for low-latency trading:
     * - Enable huge pages
     * - Lock memory
     * - Disable transparent huge pages
     * - Increase network buffers
     *
     * @return std::vector<std::string> List of settings that failed to apply
     *
     * @note Requires root privileges for most settings
     *
     * @code
     * auto failures = SystemTuning::apply_production_settings();
     * if (failures.empty()) {
     *     spdlog::info("All production settings applied successfully");
     * } else {
     *     spdlog::warn("Some settings failed - may need root:");
     *     for (const auto& failure : failures) {
     *         spdlog::warn("  - {}", failure);
     *     }
     * }
     * @endcode
     */
    static std::vector<std::string> apply_production_settings();

private:
    /**
     * @brief Check if swap is enabled
     */
    static bool is_swap_enabled();

    /**
     * @brief Get network buffer sizes
     */
    static std::pair<uint64_t, uint64_t> get_network_buffers();

    /**
     * @brief Write to sysctl file
     */
    static bool write_sysctl(const std::string& path, const std::string& value);
};

/**
 * @brief RAII helper for memory locking
 *
 * Automatically locks memory on construction and unlocks on destruction.
 *
 * @code
 * {
 *     MemoryLockGuard guard;
 *     // Memory is locked in this scope
 *     // Critical trading operations...
 * } // Memory unlocked
 * @endcode
 */
class MemoryLockGuard {
public:
    /**
     * @brief Lock memory
     */
    MemoryLockGuard();

    /**
     * @brief Unlock memory
     */
    ~MemoryLockGuard();

    /**
     * @brief Check if lock was successful
     */
    bool is_locked() const { return locked_; }

    // Disable copy and move
    MemoryLockGuard(const MemoryLockGuard&) = delete;
    MemoryLockGuard& operator=(const MemoryLockGuard&) = delete;

private:
    bool locked_{false};
    bool was_already_locked_{false};
};

} // namespace marketdata
