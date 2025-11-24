/**
 * @file system_tuning.cpp
 * @brief System tuning implementation
 */

#include "utils/system_tuning.hpp"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <cstring>

#ifdef __linux__
#include <sys/sysinfo.h>
#include <sys/resource.h>
#endif

namespace marketdata {

// ============================================================================
// NUMAAllocator Implementation
// ============================================================================

template<typename T>
T* NUMAAllocator<T>::allocate(std::size_t n) {
#ifdef __linux__
    if (numa_available() >= 0) {
        void* ptr = numa_alloc_onnode(n * sizeof(T), numa_node_);
        if (!ptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(ptr);
    }
#endif

    // Fallback to regular allocation
    return static_cast<T*>(::operator new(n * sizeof(T)));
}

template<typename T>
void NUMAAllocator<T>::deallocate(T* p, std::size_t n) noexcept {
#ifdef __linux__
    if (numa_available() >= 0) {
        numa_free(p, n * sizeof(T));
        return;
    }
#endif

    // Fallback to regular deallocation
    ::operator delete(p);
}

// Explicit instantiations for common types
template class NUMAAllocator<char>;
template class NUMAAllocator<int>;
template class NUMAAllocator<double>;
template class NUMAAllocator<uint64_t>;

// ============================================================================
// SystemTuning Implementation
// ============================================================================

SystemTuning::HugePageInfo SystemTuning::get_huge_page_info() {
    HugePageInfo info{};

#ifdef __linux__
    // Read hugepage info from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.find("HugePages_Total:") == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> info.total_2mb;
            } else if (line.find("HugePages_Free:") == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> info.available_2mb;
            } else if (line.find("Hugepagesize:") == 0) {
                std::istringstream iss(line);
                std::string key;
                uint64_t size_kb;
                iss >> key >> size_kb;
                info.page_size_2mb = size_kb * 1024;
            }
        }
    }

    // 1GB huge pages (if available)
    std::ifstream gb_total("/sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages");
    if (gb_total.is_open()) {
        gb_total >> info.total_1gb;
    }

    std::ifstream gb_free("/sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages");
    if (gb_free.is_open()) {
        gb_free >> info.available_1gb;
    }
    info.page_size_1gb = 1024ULL * 1024 * 1024;

    // Check transparent huge pages
    std::ifstream thp("/sys/kernel/mm/transparent_hugepage/enabled");
    if (thp.is_open()) {
        std::string line;
        std::getline(thp, line);
        info.transparent_enabled = (line.find("[always]") != std::string::npos);
    }
#endif

    return info;
}

bool SystemTuning::enable_huge_pages(HugePageSize size) {
#ifdef __linux__
    // This is a process-level hint - actual huge page usage depends on
    // kernel configuration

    // The madvise approach requires specific memory regions
    // For process-wide, we rely on THP or explicit allocations

    spdlog::info("SystemTuning: Huge pages enabled via allocator (kernel config required)");
    return true;
#else
    spdlog::warn("SystemTuning: Huge pages not supported on this platform");
    return false;
#endif
}

void* SystemTuning::allocate_huge_pages(size_t size, std::optional<uint32_t> numa_node) {
#ifdef __linux__
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;

    // Try 2MB huge pages first
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);

    if (ptr == MAP_FAILED) {
        spdlog::warn("SystemTuning: mmap with MAP_HUGETLB failed, trying without");
        // Fallback to regular pages
        flags &= ~MAP_HUGETLB;
        ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);

        if (ptr == MAP_FAILED) {
            spdlog::error("SystemTuning: mmap failed: {}", strerror(errno));
            return nullptr;
        }
    }

    // Bind to NUMA node if requested
    if (numa_node && numa_available() >= 0) {
        unsigned long nodemask = 1UL << *numa_node;
        if (mbind(ptr, size, MPOL_BIND, &nodemask, sizeof(nodemask) * 8, MPOL_MF_MOVE) != 0) {
            spdlog::warn("SystemTuning: mbind to NUMA node {} failed", *numa_node);
        }
    }

    // Prefault the memory
    prefault_memory(ptr, size);

    return ptr;
#else
    spdlog::warn("SystemTuning: Huge pages not supported on this platform");
    return nullptr;
#endif
}

void SystemTuning::free_huge_pages(void* ptr, size_t size) {
#ifdef __linux__
    if (ptr && size > 0) {
        munmap(ptr, size);
    }
#endif
}

bool SystemTuning::lock_memory() {
#ifdef __linux__
    // Lock all current and future memory
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        if (errno == ENOMEM) {
            spdlog::error("SystemTuning: mlockall failed - insufficient memory or RLIMIT_MEMLOCK too low");
        } else if (errno == EPERM) {
            spdlog::error("SystemTuning: mlockall failed - requires root or CAP_IPC_LOCK");
        } else {
            spdlog::error("SystemTuning: mlockall failed: {}", strerror(errno));
        }
        return false;
    }

    spdlog::info("SystemTuning: Memory locked successfully");
    return true;
#else
    spdlog::warn("SystemTuning: Memory locking not supported on this platform");
    return false;
#endif
}

bool SystemTuning::unlock_memory() {
#ifdef __linux__
    if (munlockall() != 0) {
        spdlog::error("SystemTuning: munlockall failed: {}", strerror(errno));
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool SystemTuning::is_memory_locked() {
#ifdef __linux__
    // Check if mlockall is active by reading /proc/self/status
    std::ifstream status("/proc/self/status");
    if (status.is_open()) {
        std::string line;
        while (std::getline(status, line)) {
            if (line.find("VmLck:") == 0) {
                std::istringstream iss(line);
                std::string key;
                uint64_t locked_kb;
                iss >> key >> locked_kb;
                return locked_kb > 0;
            }
        }
    }
#endif
    return false;
}

bool SystemTuning::disable_transparent_huge_pages() {
#ifdef __linux__
    // Requires root privileges
    if (!write_sysctl("/sys/kernel/mm/transparent_hugepage/enabled", "never")) {
        spdlog::warn("SystemTuning: Failed to disable THP - requires root");
        return false;
    }

    // Also disable defrag
    write_sysctl("/sys/kernel/mm/transparent_hugepage/defrag", "never");

    spdlog::info("SystemTuning: Transparent huge pages disabled");
    return true;
#else
    spdlog::warn("SystemTuning: THP control not supported on this platform");
    return false;
#endif
}

uint32_t SystemTuning::get_numa_node_count() {
#ifdef __linux__
    if (numa_available() >= 0) {
        return numa_max_node() + 1;
    }
#endif
    return 1;
}

std::vector<NUMANodeInfo> SystemTuning::get_numa_nodes() {
    std::vector<NUMANodeInfo> nodes;

#ifdef __linux__
    if (numa_available() < 0) {
        // NUMA not available - return single node
        NUMANodeInfo info{};
        info.node_id = 0;
        info.is_local = true;
        nodes.push_back(info);
        return nodes;
    }

    int max_node = numa_max_node();
    auto current_node = get_current_numa_node();

    for (int node_id = 0; node_id <= max_node; node_id++) {
        NUMANodeInfo info{};
        info.node_id = node_id;
        info.is_local = (current_node && *current_node == static_cast<uint32_t>(node_id));

        // Get memory info
        long long free_mem;
        long long total_mem = numa_node_size64(node_id, &free_mem);
        if (total_mem >= 0) {
            info.total_memory_mb = total_mem / (1024 * 1024);
            info.free_memory_mb = free_mem / (1024 * 1024);
        }

        // Get CPU cores on this node
        struct bitmask* cpus = numa_allocate_cpumask();
        if (numa_node_to_cpus(node_id, cpus) == 0) {
            for (unsigned int i = 0; i < cpus->size; i++) {
                if (numa_bitmask_isbitset(cpus, i)) {
                    info.cpu_cores.push_back(i);
                }
            }
        }
        numa_free_cpumask(cpus);

        nodes.push_back(info);
    }
#else
    // Non-NUMA system
    NUMANodeInfo info{};
    info.node_id = 0;
    info.is_local = true;
    nodes.push_back(info);
#endif

    return nodes;
}

std::optional<uint32_t> SystemTuning::get_current_numa_node() {
#ifdef __linux__
    if (numa_available() >= 0) {
        int node = numa_node_of_cpu(sched_getcpu());
        if (node >= 0) {
            return static_cast<uint32_t>(node);
        }
    }
#endif
    return std::nullopt;
}

void* SystemTuning::allocate_numa(size_t size, uint32_t numa_node) {
#ifdef __linux__
    if (numa_available() >= 0) {
        void* ptr = numa_alloc_onnode(size, numa_node);
        if (ptr) {
            prefault_memory(ptr, size);
        }
        return ptr;
    }
#endif

    // Fallback to regular allocation
    void* ptr = malloc(size);
    if (ptr) {
        prefault_memory(ptr, size);
    }
    return ptr;
}

void SystemTuning::free_numa(void* ptr, size_t size) {
#ifdef __linux__
    if (numa_available() >= 0) {
        numa_free(ptr, size);
        return;
    }
#endif
    free(ptr);
}

void SystemTuning::prefault_memory(void* ptr, size_t size) {
    if (!ptr || size == 0) {
        return;
    }

    // Touch every page to ensure it's resident
    const size_t page_size = 4096;
    volatile char* p = static_cast<volatile char*>(ptr);

    for (size_t i = 0; i < size; i += page_size) {
        p[i] = p[i];  // Read and write back
    }

    // Touch last byte
    if (size > 0) {
        p[size - 1] = p[size - 1];
    }
}

MemoryStats SystemTuning::get_memory_stats() {
    MemoryStats stats{};

#ifdef __linux__
    // Read from /proc/self/status
    std::ifstream status("/proc/self/status");
    if (status.is_open()) {
        std::string line;
        while (std::getline(status, line)) {
            if (line.find("VmRSS:") == 0) {
                std::istringstream iss(line);
                std::string key;
                uint64_t rss_kb;
                iss >> key >> rss_kb;
                stats.allocated_bytes = rss_kb * 1024;
            } else if (line.find("VmLck:") == 0) {
                std::istringstream iss(line);
                std::string key;
                uint64_t locked_kb;
                iss >> key >> locked_kb;
                stats.is_locked = (locked_kb > 0);
            }
        }
    }

    // Check if using huge pages
    std::ifstream smaps("/proc/self/smaps");
    if (smaps.is_open()) {
        std::string line;
        while (std::getline(smaps, line)) {
            if (line.find("AnonHugePages:") != std::string::npos) {
                std::istringstream iss(line);
                std::string key;
                uint64_t huge_kb;
                iss >> key >> huge_kb;
                if (huge_kb > 0) {
                    stats.uses_huge_pages = true;
                    stats.huge_pages_allocated += huge_kb / 2048;  // 2MB pages
                }
            }
        }
    }

    auto current_node = get_current_numa_node();
    if (current_node) {
        stats.numa_node = *current_node;
    }
#endif

    return stats;
}

bool SystemTuning::set_network_buffers(uint64_t rmem_max, uint64_t wmem_max) {
#ifdef __linux__
    bool success = true;

    if (!write_sysctl("/proc/sys/net/core/rmem_max", std::to_string(rmem_max))) {
        spdlog::warn("SystemTuning: Failed to set rmem_max - requires root");
        success = false;
    }

    if (!write_sysctl("/proc/sys/net/core/wmem_max", std::to_string(wmem_max))) {
        spdlog::warn("SystemTuning: Failed to set wmem_max - requires root");
        success = false;
    }

    if (success) {
        spdlog::info("SystemTuning: Network buffers set to {} / {}", rmem_max, wmem_max);
    }

    return success;
#else
    spdlog::warn("SystemTuning: Network buffer tuning not supported on this platform");
    return false;
#endif
}

bool SystemTuning::disable_aslr() {
#ifdef __linux__
    if (!write_sysctl("/proc/sys/kernel/randomize_va_space", "0")) {
        spdlog::warn("SystemTuning: Failed to disable ASLR - requires root");
        return false;
    }

    spdlog::warn("SystemTuning: ASLR disabled - security impact!");
    return true;
#else
    spdlog::warn("SystemTuning: ASLR control not supported on this platform");
    return false;
#endif
}

std::vector<std::string> SystemTuning::validate_production_config() {
    std::vector<std::string> warnings;

    // Check huge pages
    auto hp_info = get_huge_page_info();
    if (hp_info.total_2mb == 0) {
        warnings.push_back("No huge pages configured - set /proc/sys/vm/nr_hugepages");
    }

    if (hp_info.transparent_enabled) {
        warnings.push_back("Transparent huge pages enabled - causes latency spikes, disable with 'echo never > /sys/kernel/mm/transparent_hugepage/enabled'");
    }

    // Check memory locking
    if (!is_memory_locked()) {
        warnings.push_back("Memory not locked - enable with mlockall() or check RLIMIT_MEMLOCK");
    }

    // Check swap
    if (is_swap_enabled()) {
        warnings.push_back("Swap is enabled - disable with 'swapoff -a' for trading");
    }

    // Check NUMA
    if (get_numa_node_count() > 1) {
        auto nodes = get_numa_nodes();
        spdlog::info("NUMA system with {} nodes - ensure process affinity and memory locality", nodes.size());
    }

    // Check network buffers
    auto [rmem, wmem] = get_network_buffers();
    const uint64_t recommended_size = 16 * 1024 * 1024;  // 16MB
    if (rmem < recommended_size || wmem < recommended_size) {
        warnings.push_back("Network buffers too small - recommend 16MB+ (current: rmem=" +
                          std::to_string(rmem) + ", wmem=" + std::to_string(wmem) + ")");
    }

    return warnings;
}

std::vector<std::string> SystemTuning::apply_production_settings() {
    std::vector<std::string> failures;

    // Enable huge pages
    if (!enable_huge_pages(HugePageSize::HUGE_2MB)) {
        failures.push_back("Failed to enable huge pages");
    }

    // Lock memory
    if (!lock_memory()) {
        failures.push_back("Failed to lock memory (requires root or CAP_IPC_LOCK)");
    }

    // Disable transparent huge pages
    if (!disable_transparent_huge_pages()) {
        failures.push_back("Failed to disable THP (requires root)");
    }

    // Set network buffers to 16MB
    if (!set_network_buffers(16 * 1024 * 1024, 16 * 1024 * 1024)) {
        failures.push_back("Failed to set network buffers (requires root)");
    }

    return failures;
}

bool SystemTuning::is_swap_enabled() {
#ifdef __linux__
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.find("SwapTotal:") == 0) {
                std::istringstream iss(line);
                std::string key;
                uint64_t swap_kb;
                iss >> key >> swap_kb;
                return (swap_kb > 0);
            }
        }
    }
#endif
    return false;
}

std::pair<uint64_t, uint64_t> SystemTuning::get_network_buffers() {
    uint64_t rmem = 0, wmem = 0;

#ifdef __linux__
    std::ifstream rmem_file("/proc/sys/net/core/rmem_max");
    if (rmem_file.is_open()) {
        rmem_file >> rmem;
    }

    std::ifstream wmem_file("/proc/sys/net/core/wmem_max");
    if (wmem_file.is_open()) {
        wmem_file >> wmem;
    }
#endif

    return {rmem, wmem};
}

bool SystemTuning::write_sysctl(const std::string& path, const std::string& value) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }

    file << value;
    file.close();

    return !file.fail();
}

// ============================================================================
// MemoryLockGuard Implementation
// ============================================================================

MemoryLockGuard::MemoryLockGuard() {
    was_already_locked_ = SystemTuning::is_memory_locked();

    if (!was_already_locked_) {
        locked_ = SystemTuning::lock_memory();
    } else {
        locked_ = true;  // Already locked
    }
}

MemoryLockGuard::~MemoryLockGuard() {
    if (locked_ && !was_already_locked_) {
        SystemTuning::unlock_memory();
    }
}

} // namespace marketdata
