/**
 * @file cpu_affinity.cpp
 * @brief CPU affinity implementation
 */

#include "utils/cpu_affinity.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <sys/syscall.h>
#include <numa.h>
#endif

namespace marketdata {

// ============================================================================
// CPUAffinityMask Implementation
// ============================================================================

CPUAffinityMask::CPUAffinityMask(const std::vector<uint32_t>& cores)
    : cores_(cores) {
    // Sort and remove duplicates
    std::sort(cores_.begin(), cores_.end());
    cores_.erase(std::unique(cores_.begin(), cores_.end()), cores_.end());
}

void CPUAffinityMask::add_core(uint32_t core_id) {
    if (!has_core(core_id)) {
        cores_.push_back(core_id);
        std::sort(cores_.begin(), cores_.end());
    }
}

void CPUAffinityMask::remove_core(uint32_t core_id) {
    cores_.erase(std::remove(cores_.begin(), cores_.end(), core_id), cores_.end());
}

bool CPUAffinityMask::has_core(uint32_t core_id) const {
    return std::find(cores_.begin(), cores_.end(), core_id) != cores_.end();
}

std::vector<uint32_t> CPUAffinityMask::get_cores() const {
    return cores_;
}

void CPUAffinityMask::clear() {
    cores_.clear();
}

// ============================================================================
// CPUAffinity Implementation
// ============================================================================

uint32_t CPUAffinity::get_core_count() {
    return std::thread::hardware_concurrency();
}

std::vector<CPUCoreInfo> CPUAffinity::get_available_cores() {
    std::vector<CPUCoreInfo> cores;
    uint32_t core_count = get_core_count();

#ifdef __linux__
    // Try to get detailed information from /sys
    for (uint32_t i = 0; i < core_count; i++) {
        CPUCoreInfo info;
        info.core_id = i;
        info.is_online = true;
        info.is_isolated = false;
        info.numa_node = 0;
        info.frequency_mhz = 0;

        // Check if core is online
        std::string online_path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/online";
        std::ifstream online_file(online_path);
        if (online_file.is_open()) {
            int online;
            online_file >> online;
            info.is_online = (online == 1);
        }

        // Get NUMA node (if available)
        if (numa_available() >= 0) {
            info.numa_node = numa_node_of_cpu(i);
        }

        // Get current frequency
        std::string freq_path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq";
        std::ifstream freq_file(freq_path);
        if (freq_file.is_open()) {
            uint64_t freq_khz;
            freq_file >> freq_khz;
            info.frequency_mhz = freq_khz / 1000;
        }

        cores.push_back(info);
    }

    // Mark isolated cores
    auto isolated = parse_isolcpus();
    for (auto core_id : isolated) {
        if (core_id < cores.size()) {
            cores[core_id].is_isolated = true;
        }
    }

#else
    // Windows or other platforms - basic information only
    for (uint32_t i = 0; i < core_count; i++) {
        CPUCoreInfo info;
        info.core_id = i;
        info.is_online = true;
        info.is_isolated = false;
        info.numa_node = 0;
        info.frequency_mhz = 0;
        cores.push_back(info);
    }
#endif

    return cores;
}

std::vector<CPUCoreInfo> CPUAffinity::get_isolated_cores() {
    std::vector<CPUCoreInfo> isolated_cores;

#ifdef __linux__
    auto all_cores = get_available_cores();
    for (const auto& core : all_cores) {
        if (core.is_isolated) {
            isolated_cores.push_back(core);
        }
    }
#endif

    return isolated_cores;
}

bool CPUAffinity::set_thread_affinity(uint32_t core_id) {
    CPUAffinityMask mask({core_id});
    return set_thread_affinity(mask);
}

bool CPUAffinity::set_thread_affinity(const CPUAffinityMask& mask) {
    if (mask.empty()) {
        spdlog::warn("CPUAffinity: Cannot set empty affinity mask");
        return false;
    }

#ifdef _WIN32
    // Windows implementation
    DWORD_PTR affinity_mask = 0;
    for (auto core_id : mask.get_cores()) {
        if (core_id >= 64) {
            spdlog::warn("CPUAffinity: Core ID {} exceeds Windows limit (64)", core_id);
            continue;
        }
        affinity_mask |= (1ULL << core_id);
    }

    if (affinity_mask == 0) {
        return false;
    }

    DWORD_PTR result = SetThreadAffinityMask(GetCurrentThread(), affinity_mask);
    if (result == 0) {
        spdlog::error("CPUAffinity: SetThreadAffinityMask failed: {}", GetLastError());
        return false;
    }

    return true;

#elif defined(__linux__)
    // Linux implementation
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (auto core_id : mask.get_cores()) {
        if (core_id >= CPU_SETSIZE) {
            spdlog::warn("CPUAffinity: Core ID {} exceeds CPU_SETSIZE ({})", core_id, CPU_SETSIZE);
            continue;
        }
        CPU_SET(core_id, &cpuset);
    }

    int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        spdlog::error("CPUAffinity: pthread_setaffinity_np failed: {}", result);
        return false;
    }

    return true;

#else
    spdlog::warn("CPUAffinity: Thread affinity not supported on this platform");
    return false;
#endif
}

bool CPUAffinity::set_thread_affinity(std::thread& thread, uint32_t core_id) {
    CPUAffinityMask mask({core_id});
    return set_thread_affinity(thread, mask);
}

bool CPUAffinity::set_thread_affinity(std::thread& thread, const CPUAffinityMask& mask) {
    if (mask.empty()) {
        spdlog::warn("CPUAffinity: Cannot set empty affinity mask");
        return false;
    }

#ifdef _WIN32
    // Windows implementation
    DWORD_PTR affinity_mask = 0;
    for (auto core_id : mask.get_cores()) {
        if (core_id >= 64) {
            spdlog::warn("CPUAffinity: Core ID {} exceeds Windows limit (64)", core_id);
            continue;
        }
        affinity_mask |= (1ULL << core_id);
    }

    if (affinity_mask == 0) {
        return false;
    }

    HANDLE thread_handle = static_cast<HANDLE>(thread.native_handle());
    DWORD_PTR result = SetThreadAffinityMask(thread_handle, affinity_mask);
    if (result == 0) {
        spdlog::error("CPUAffinity: SetThreadAffinityMask failed: {}", GetLastError());
        return false;
    }

    return true;

#elif defined(__linux__)
    // Linux implementation
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (auto core_id : mask.get_cores()) {
        if (core_id >= CPU_SETSIZE) {
            spdlog::warn("CPUAffinity: Core ID {} exceeds CPU_SETSIZE ({})", core_id, CPU_SETSIZE);
            continue;
        }
        CPU_SET(core_id, &cpuset);
    }

    pthread_t native_handle = thread.native_handle();
    int result = pthread_setaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        spdlog::error("CPUAffinity: pthread_setaffinity_np failed: {}", result);
        return false;
    }

    return true;

#else
    spdlog::warn("CPUAffinity: Thread affinity not supported on this platform");
    return false;
#endif
}

std::optional<CPUAffinityMask> CPUAffinity::get_thread_affinity() {
#ifdef _WIN32
    // Windows doesn't provide an easy way to query affinity
    spdlog::warn("CPUAffinity: get_thread_affinity not supported on Windows");
    return std::nullopt;

#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    int result = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        spdlog::error("CPUAffinity: pthread_getaffinity_np failed: {}", result);
        return std::nullopt;
    }

    std::vector<uint32_t> cores;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &cpuset)) {
            cores.push_back(i);
        }
    }

    return CPUAffinityMask(cores);

#else
    spdlog::warn("CPUAffinity: get_thread_affinity not supported on this platform");
    return std::nullopt;
#endif
}

std::optional<CPUAffinityMask> CPUAffinity::get_thread_affinity(std::thread& thread) {
#ifdef _WIN32
    spdlog::warn("CPUAffinity: get_thread_affinity not supported on Windows");
    return std::nullopt;

#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    pthread_t native_handle = thread.native_handle();
    int result = pthread_getaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        spdlog::error("CPUAffinity: pthread_getaffinity_np failed: {}", result);
        return std::nullopt;
    }

    std::vector<uint32_t> cores;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &cpuset)) {
            cores.push_back(i);
        }
    }

    return CPUAffinityMask(cores);

#else
    spdlog::warn("CPUAffinity: get_thread_affinity not supported on this platform");
    return std::nullopt;
#endif
}

bool CPUAffinity::set_thread_priority(SchedulingPolicy policy, int priority) {
#ifdef _WIN32
    // Windows implementation
    HANDLE thread_handle = GetCurrentThread();
    int win_priority;

    switch (policy) {
        case SchedulingPolicy::REALTIME_FIFO:
        case SchedulingPolicy::REALTIME_RR:
            win_priority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        case SchedulingPolicy::NORMAL:
            win_priority = THREAD_PRIORITY_NORMAL;
            break;
        case SchedulingPolicy::BATCH:
            win_priority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case SchedulingPolicy::IDLE:
            win_priority = THREAD_PRIORITY_IDLE;
            break;
        default:
            win_priority = THREAD_PRIORITY_NORMAL;
    }

    if (!SetThreadPriority(thread_handle, win_priority)) {
        spdlog::error("CPUAffinity: SetThreadPriority failed: {}", GetLastError());
        return false;
    }

    return true;

#elif defined(__linux__)
    // Linux implementation
    struct sched_param param;
    param.sched_priority = priority;

    int linux_policy;
    switch (policy) {
        case SchedulingPolicy::REALTIME_FIFO:
            linux_policy = SCHED_FIFO;
            break;
        case SchedulingPolicy::REALTIME_RR:
            linux_policy = SCHED_RR;
            break;
        case SchedulingPolicy::BATCH:
            linux_policy = SCHED_BATCH;
            param.sched_priority = 0;
            break;
        case SchedulingPolicy::IDLE:
            linux_policy = SCHED_IDLE;
            param.sched_priority = 0;
            break;
        case SchedulingPolicy::NORMAL:
        default:
            linux_policy = SCHED_OTHER;
            param.sched_priority = 0;
    }

    int result = pthread_setschedparam(pthread_self(), linux_policy, &param);
    if (result != 0) {
        if (result == EPERM) {
            spdlog::warn("CPUAffinity: pthread_setschedparam failed: Permission denied (requires root for RT)");
        } else {
            spdlog::error("CPUAffinity: pthread_setschedparam failed: {}", result);
        }
        return false;
    }

    return true;

#else
    spdlog::warn("CPUAffinity: set_thread_priority not supported on this platform");
    return false;
#endif
}

bool CPUAffinity::set_thread_priority(std::thread& thread, SchedulingPolicy policy, int priority) {
#ifdef _WIN32
    // Windows implementation
    HANDLE thread_handle = static_cast<HANDLE>(thread.native_handle());
    int win_priority;

    switch (policy) {
        case SchedulingPolicy::REALTIME_FIFO:
        case SchedulingPolicy::REALTIME_RR:
            win_priority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        case SchedulingPolicy::NORMAL:
            win_priority = THREAD_PRIORITY_NORMAL;
            break;
        case SchedulingPolicy::BATCH:
            win_priority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case SchedulingPolicy::IDLE:
            win_priority = THREAD_PRIORITY_IDLE;
            break;
        default:
            win_priority = THREAD_PRIORITY_NORMAL;
    }

    if (!SetThreadPriority(thread_handle, win_priority)) {
        spdlog::error("CPUAffinity: SetThreadPriority failed: {}", GetLastError());
        return false;
    }

    return true;

#elif defined(__linux__)
    // Linux implementation
    struct sched_param param;
    param.sched_priority = priority;

    int linux_policy;
    switch (policy) {
        case SchedulingPolicy::REALTIME_FIFO:
            linux_policy = SCHED_FIFO;
            break;
        case SchedulingPolicy::REALTIME_RR:
            linux_policy = SCHED_RR;
            break;
        case SchedulingPolicy::BATCH:
            linux_policy = SCHED_BATCH;
            param.sched_priority = 0;
            break;
        case SchedulingPolicy::IDLE:
            linux_policy = SCHED_IDLE;
            param.sched_priority = 0;
            break;
        case SchedulingPolicy::NORMAL:
        default:
            linux_policy = SCHED_OTHER;
            param.sched_priority = 0;
    }

    pthread_t native_handle = thread.native_handle();
    int result = pthread_setschedparam(native_handle, linux_policy, &param);
    if (result != 0) {
        if (result == EPERM) {
            spdlog::warn("CPUAffinity: pthread_setschedparam failed: Permission denied (requires root for RT)");
        } else {
            spdlog::error("CPUAffinity: pthread_setschedparam failed: {}", result);
        }
        return false;
    }

    return true;

#else
    spdlog::warn("CPUAffinity: set_thread_priority not supported on this platform");
    return false;
#endif
}

bool CPUAffinity::is_on_isolated_core() {
#ifdef __linux__
    auto current_core = get_current_core();
    if (!current_core) {
        return false;
    }

    auto isolated = parse_isolcpus();
    return std::find(isolated.begin(), isolated.end(), *current_core) != isolated.end();
#else
    return false;
#endif
}

std::optional<uint32_t> CPUAffinity::get_current_core() {
#ifdef __linux__
    int cpu = sched_getcpu();
    if (cpu < 0) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(cpu);
#else
    // Not easily available on Windows
    return std::nullopt;
#endif
}

std::vector<std::string> CPUAffinity::validate_configuration() {
    std::vector<std::string> warnings;

    // Check core count
    uint32_t core_count = get_core_count();
    if (core_count < 4) {
        warnings.push_back("System has only " + std::to_string(core_count) + " cores - recommend at least 4 for trading");
    }

#ifdef __linux__
    // Check for isolated cores
    auto isolated = get_isolated_cores();
    if (isolated.empty()) {
        warnings.push_back("No isolated cores found - add isolcpus= to kernel cmdline for best latency");
    } else {
        spdlog::info("Found {} isolated cores", isolated.size());
    }

    // Check CPU governor
    std::string governor = get_cpu_governor();
    if (governor != "performance") {
        warnings.push_back("CPU governor is '" + governor + "' - should be 'performance' for trading");
    }

    // Check frequency scaling
    if (!is_frequency_scaling_disabled()) {
        warnings.push_back("CPU frequency scaling is enabled - disable for consistent latency");
    }

    // Check NUMA
    if (numa_available() >= 0) {
        int numa_nodes = numa_max_node() + 1;
        if (numa_nodes > 1) {
            spdlog::info("NUMA system detected with {} nodes", numa_nodes);
            warnings.push_back("NUMA system - ensure memory allocated on same node as pinned CPU cores");
        }
    }
#endif

    return warnings;
}

std::vector<uint32_t> CPUAffinity::parse_isolcpus() {
    std::vector<uint32_t> isolated;

#ifdef __linux__
    std::ifstream cmdline("/proc/cmdline");
    if (!cmdline.is_open()) {
        return isolated;
    }

    std::string line;
    std::getline(cmdline, line);

    // Find isolcpus= parameter
    size_t pos = line.find("isolcpus=");
    if (pos == std::string::npos) {
        return isolated;
    }

    // Extract the value
    pos += 9;  // Length of "isolcpus="
    size_t end = line.find(' ', pos);
    std::string isolcpus_value = line.substr(pos, end - pos);

    // Parse comma-separated list of cores/ranges
    std::istringstream iss(isolcpus_value);
    std::string token;

    while (std::getline(iss, token, ',')) {
        // Check if it's a range (e.g., "2-5")
        size_t dash = token.find('-');
        if (dash != std::string::npos) {
            uint32_t start = std::stoi(token.substr(0, dash));
            uint32_t end_core = std::stoi(token.substr(dash + 1));
            for (uint32_t i = start; i <= end_core; i++) {
                isolated.push_back(i);
            }
        } else {
            isolated.push_back(std::stoi(token));
        }
    }
#endif

    return isolated;
}

bool CPUAffinity::is_frequency_scaling_disabled() {
#ifdef __linux__
    std::string governor = get_cpu_governor();
    return (governor == "performance");
#else
    return false;
#endif
}

std::string CPUAffinity::get_cpu_governor() {
#ifdef __linux__
    std::ifstream governor_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (governor_file.is_open()) {
        std::string governor;
        std::getline(governor_file, governor);
        return governor;
    }
#endif
    return "unknown";
}

// ============================================================================
// ThreadAffinityGuard Implementation
// ============================================================================

ThreadAffinityGuard::ThreadAffinityGuard(uint32_t core_id) {
    // Save current affinity
    original_affinity_ = CPUAffinity::get_thread_affinity();

    // Set new affinity
    if (!CPUAffinity::set_thread_affinity(core_id)) {
        spdlog::warn("ThreadAffinityGuard: Failed to set affinity to core {}", core_id);
        restore_on_destroy_ = false;
    }
}

ThreadAffinityGuard::ThreadAffinityGuard(const CPUAffinityMask& mask) {
    // Save current affinity
    original_affinity_ = CPUAffinity::get_thread_affinity();

    // Set new affinity
    if (!CPUAffinity::set_thread_affinity(mask)) {
        spdlog::warn("ThreadAffinityGuard: Failed to set affinity");
        restore_on_destroy_ = false;
    }
}

ThreadAffinityGuard::~ThreadAffinityGuard() {
    if (restore_on_destroy_ && original_affinity_) {
        CPUAffinity::set_thread_affinity(*original_affinity_);
    }
}

} // namespace marketdata
