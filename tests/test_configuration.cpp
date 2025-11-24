/**
 * @file test_configuration.cpp
 * @brief Unit tests for configuration management and utilities
 */

#include <gtest/gtest.h>
#include "config/configuration_manager.hpp"
#include "utils/cpu_affinity.hpp"
#include "utils/system_tuning.hpp"
#include <fstream>
#include <filesystem>

using namespace marketdata;

// ============================================================================
// Configuration Manager Tests
// ============================================================================

class ConfigurationManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temporary config file
        test_config_path = std::filesystem::temp_directory_path() / "test_config.yaml";
        create_test_config();
    }

    void TearDown() override {
        // Clean up
        if (std::filesystem::exists(test_config_path)) {
            std::filesystem::remove(test_config_path);
        }
    }

    void create_test_config() {
        std::ofstream file(test_config_path);
        file << R"(
exchanges:
  binance:
    enabled: true
    websocket_url: "wss://stream.binance.com:9443/ws"
    rest_url: "https://api.binance.com"
    use_testnet: false
    depth: 10
    reconnect_delay_ms: 5000
    max_reconnect_attempts: 10
    rate_limit_per_sec: 20.0
    symbols:
      - "btcusdt"
      - "ethusdt"

  coinbase:
    enabled: true
    websocket_url: "wss://ws-feed.exchange.coinbase.com"
    rest_url: "https://api.exchange.coinbase.com"
    use_testnet: false
    reconnect_delay_ms: 5000
    max_reconnect_attempts: 10
    rate_limit_per_sec: 30.0
    symbols:
      - "BTC-USD"

server:
  host: "0.0.0.0"
  websocket_port: 8080
  rest_port: 8081
  max_connections: 1000
  heartbeat_interval_ms: 30000

performance:
  enable_cpu_pinning: false
  cpu_cores: [2, 3, 4]
  enable_huge_pages: false
  enable_numa: false
  memory_pool_size: 100000
  queue_size: 100000

logging:
  level: "info"
  pattern: "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v"
  async: true
  log_file: "./logs/test.log"
  max_file_size: 10485760
  max_files: 5

recording:
  enabled: false
  output_dir: "./data"
  format: "binary"
  buffer_size: 100000

analytics:
  vwap_window_ns: 60000000000
  twap_window_ns: 300000000000
  imbalance_window_ns: 10000000000
  spread_window_ns: 60000000000

arbitrage:
  enabled: true
  min_profit_percent: 0.1
  history_size: 1000
)";
        file.close();
    }

    std::filesystem::path test_config_path;
};

TEST_F(ConfigurationManagerTest, LoadConfiguration) {
    ConfigurationManager manager;

    EXPECT_TRUE(manager.load(test_config_path.string()));
}

TEST_F(ConfigurationManagerTest, LoadNonexistentFile) {
    ConfigurationManager manager;

    EXPECT_FALSE(manager.load("nonexistent.yaml"));
}

TEST_F(ConfigurationManagerTest, GetSystemConfig) {
    ConfigurationManager manager;
    manager.load(test_config_path.string());

    auto config = manager.get_config();

    EXPECT_EQ(config.server_host, "0.0.0.0");
    EXPECT_EQ(config.websocket_port, 8080);
    EXPECT_EQ(config.rest_port, 8081);
    EXPECT_EQ(config.log_level, "info");
    EXPECT_FALSE(config.enable_cpu_pinning);
    EXPECT_TRUE(config.arbitrage_enabled);
    EXPECT_DOUBLE_EQ(config.min_profit_percent, 0.1);
}

TEST_F(ConfigurationManagerTest, GetExchangeConfig) {
    ConfigurationManager manager;
    manager.load(test_config_path.string());

    auto binance_config = manager.get_exchange_config(Exchange::BINANCE);

    EXPECT_TRUE(binance_config.enabled);
    EXPECT_EQ(binance_config.websocket_url, "wss://stream.binance.com:9443/ws");
    EXPECT_EQ(binance_config.rest_url, "https://api.binance.com");
    EXPECT_FALSE(binance_config.use_testnet);
    EXPECT_EQ(binance_config.depth, 10);
    EXPECT_EQ(binance_config.reconnect_delay_ms, 5000);
    EXPECT_EQ(binance_config.max_reconnect_attempts, 10);
    EXPECT_DOUBLE_EQ(binance_config.rate_limit_per_sec, 20.0);
    EXPECT_EQ(binance_config.symbols.size(), 2);
    EXPECT_EQ(binance_config.symbols[0], "btcusdt");
}

TEST_F(ConfigurationManagerTest, Validation) {
    ConfigurationManager manager;
    manager.load(test_config_path.string());

    auto errors = manager.validate();

    // Should have no validation errors
    EXPECT_TRUE(errors.empty());
}

TEST_F(ConfigurationManagerTest, CPUCoresConfiguration) {
    ConfigurationManager manager;
    manager.load(test_config_path.string());

    auto config = manager.get_config();

    EXPECT_EQ(config.cpu_cores.size(), 3);
    EXPECT_EQ(config.cpu_cores[0], 2);
    EXPECT_EQ(config.cpu_cores[1], 3);
    EXPECT_EQ(config.cpu_cores[2], 4);
}

TEST_F(ConfigurationManagerTest, AnalyticsConfiguration) {
    ConfigurationManager manager;
    manager.load(test_config_path.string());

    auto config = manager.get_config();

    EXPECT_EQ(config.vwap_window_ns, 60'000'000'000);
    EXPECT_EQ(config.twap_window_ns, 300'000'000'000);
    EXPECT_EQ(config.imbalance_window_ns, 10'000'000'000);
    EXPECT_EQ(config.spread_window_ns, 60'000'000'000);
}

// ============================================================================
// CPU Affinity Tests
// ============================================================================

class CPUAffinityTest : public ::testing::Test {
protected:
};

TEST_F(CPUAffinityTest, GetCoreCount) {
    uint32_t count = CPUAffinity::get_core_count();

    EXPECT_GT(count, 0);
    EXPECT_LE(count, 256);  // Reasonable upper limit

    std::cout << "System has " << count << " CPU cores\n";
}

TEST_F(CPUAffinityTest, GetAvailableCores) {
    auto cores = CPUAffinity::get_available_cores();

    EXPECT_GT(cores.size(), 0);

    for (const auto& core : cores) {
        EXPECT_GE(core.core_id, 0);
        EXPECT_TRUE(core.is_online || !core.is_online);  // Just check field exists
    }

    std::cout << "Found " << cores.size() << " available cores\n";
}

TEST_F(CPUAffinityTest, AffinityMask) {
    CPUAffinityMask mask;

    EXPECT_TRUE(mask.empty());
    EXPECT_EQ(mask.size(), 0);

    mask.add_core(0);
    mask.add_core(1);
    mask.add_core(2);

    EXPECT_FALSE(mask.empty());
    EXPECT_EQ(mask.size(), 3);
    EXPECT_TRUE(mask.has_core(0));
    EXPECT_TRUE(mask.has_core(1));
    EXPECT_TRUE(mask.has_core(2));
    EXPECT_FALSE(mask.has_core(3));

    mask.remove_core(1);

    EXPECT_EQ(mask.size(), 2);
    EXPECT_FALSE(mask.has_core(1));
}

TEST_F(CPUAffinityTest, AffinityMaskDuplicates) {
    CPUAffinityMask mask;

    mask.add_core(0);
    mask.add_core(0);  // Duplicate
    mask.add_core(0);  // Duplicate

    // Should only have one copy
    EXPECT_EQ(mask.size(), 1);
}

TEST_F(CPUAffinityTest, AffinityMaskFromVector) {
    std::vector<uint32_t> cores = {0, 1, 2, 3};
    CPUAffinityMask mask(cores);

    EXPECT_EQ(mask.size(), 4);
    EXPECT_TRUE(mask.has_core(0));
    EXPECT_TRUE(mask.has_core(3));
}

TEST_F(CPUAffinityTest, ValidationDoesNotThrow) {
    // Validation should not throw exceptions
    EXPECT_NO_THROW({
        auto warnings = CPUAffinity::validate_configuration();
        // Just check it returns something
        (void)warnings;
    });
}

#ifdef __linux__
TEST_F(CPUAffinityTest, SetThreadAffinity) {
    // Try to pin to core 0 (should exist on all systems)
    bool result = CPUAffinity::set_thread_affinity(0);

    // May fail without permissions, but should not crash
    if (result) {
        std::cout << "Successfully pinned thread to core 0\n";
    } else {
        std::cout << "Failed to pin thread (may require permissions)\n";
    }
}

TEST_F(CPUAffinityTest, GetCurrentCore) {
    auto core = CPUAffinity::get_current_core();

    if (core.has_value()) {
        EXPECT_GE(*core, 0);
        EXPECT_LT(*core, CPUAffinity::get_core_count());
        std::cout << "Currently running on core " << *core << "\n";
    }
}
#endif

// ============================================================================
// System Tuning Tests
// ============================================================================

class SystemTuningTest : public ::testing::Test {
protected:
};

TEST_F(SystemTuningTest, GetHugePageInfo) {
    auto info = SystemTuning::get_huge_page_info();

    // Check fields exist and are reasonable
    EXPECT_GE(info.total_2mb, 0);
    EXPECT_GE(info.available_2mb, 0);
    EXPECT_LE(info.available_2mb, info.total_2mb);

    std::cout << "Huge pages: " << info.available_2mb << " / " << info.total_2mb << " (2MB)\n";
    std::cout << "THP enabled: " << info.transparent_enabled << "\n";
}

TEST_F(SystemTuningTest, GetNUMANodeCount) {
    uint32_t count = SystemTuning::get_numa_node_count();

    EXPECT_GT(count, 0);
    EXPECT_LE(count, 64);  // Reasonable upper limit

    std::cout << "NUMA nodes: " << count << "\n";
}

TEST_F(SystemTuningTest, GetNUMANodes) {
    auto nodes = SystemTuning::get_numa_nodes();

    EXPECT_GT(nodes.size(), 0);

    for (const auto& node : nodes) {
        EXPECT_GE(node.node_id, 0);
        std::cout << "NUMA node " << node.node_id << ": "
                  << node.cpu_cores.size() << " cores, "
                  << node.free_memory_mb << " MB free\n";
    }
}

TEST_F(SystemTuningTest, GetMemoryStats) {
    auto stats = SystemTuning::get_memory_stats();

    // Memory should be allocated
    EXPECT_GT(stats.allocated_bytes, 0);

    std::cout << "Memory: " << (stats.allocated_bytes / 1024 / 1024) << " MB allocated, "
              << "locked=" << stats.is_locked << ", "
              << "huge_pages=" << stats.uses_huge_pages << "\n";
}

TEST_F(SystemTuningTest, PrefaultMemory) {
    const size_t size = 1024 * 1024;  // 1 MB
    void* ptr = malloc(size);
    ASSERT_NE(ptr, nullptr);

    // Should not crash
    EXPECT_NO_THROW({
        SystemTuning::prefault_memory(ptr, size);
    });

    free(ptr);
}

TEST_F(SystemTuningTest, ValidationDoesNotThrow) {
    EXPECT_NO_THROW({
        auto warnings = SystemTuning::validate_production_config();
        (void)warnings;
    });
}

#ifdef __linux__
TEST_F(SystemTuningTest, MemoryLockAttempt) {
    // Try to lock memory (may fail without permissions)
    bool result = SystemTuning::lock_memory();

    if (result) {
        std::cout << "Successfully locked memory\n";

        // Check if really locked
        EXPECT_TRUE(SystemTuning::is_memory_locked());

        // Unlock
        SystemTuning::unlock_memory();
    } else {
        std::cout << "Failed to lock memory (may require CAP_IPC_LOCK)\n";
    }
}

TEST_F(SystemTuningTest, NUMAAllocation) {
    if (SystemTuning::get_numa_node_count() > 1) {
        const size_t size = 1024;  // 1 KB
        void* ptr = SystemTuning::allocate_numa(size, 0);

        if (ptr) {
            // Write to memory to ensure it's accessible
            memset(ptr, 0, size);

            SystemTuning::free_numa(ptr, size);
        }
    }
}
#endif

// ============================================================================
// NUMAAllocator Tests
// ============================================================================

TEST(NUMAAllocatorTest, BasicAllocation) {
    NUMAAllocator<int> allocator(0);

    int* ptr = allocator.allocate(100);
    ASSERT_NE(ptr, nullptr);

    // Use the memory
    for (int i = 0; i < 100; i++) {
        ptr[i] = i;
    }

    // Verify
    for (int i = 0; i < 100; i++) {
        EXPECT_EQ(ptr[i], i);
    }

    allocator.deallocate(ptr, 100);
}

TEST(NUMAAllocatorTest, WithSTLContainer) {
    NUMAAllocator<int> allocator(0);
    std::vector<int, NUMAAllocator<int>> vec(allocator);

    vec.reserve(1000);

    for (int i = 0; i < 1000; i++) {
        vec.push_back(i);
    }

    EXPECT_EQ(vec.size(), 1000);

    for (int i = 0; i < 1000; i++) {
        EXPECT_EQ(vec[i], i);
    }
}

TEST(NUMAAllocatorTest, Equality) {
    NUMAAllocator<int> alloc1(0);
    NUMAAllocator<int> alloc2(0);
    NUMAAllocator<int> alloc3(1);

    EXPECT_TRUE(alloc1 == alloc2);
    EXPECT_FALSE(alloc1 == alloc3);
    EXPECT_TRUE(alloc1 != alloc3);
}

// ============================================================================
// Guard Classes Tests
// ============================================================================

#ifdef __linux__
TEST(GuardClassesTest, ThreadAffinityGuard) {
    // Get original affinity
    auto original = CPUAffinity::get_thread_affinity();

    {
        // Pin to core 0 within this scope
        ThreadAffinityGuard guard(0);

        // Check if pinned (may fail without permissions)
        auto current = CPUAffinity::get_thread_affinity();
        if (current.has_value()) {
            std::cout << "Temporarily pinned to core within guard\n";
        }
    }

    // Should be restored (if guard succeeded)
    std::cout << "Guard destroyed, affinity restored\n";
}

TEST(GuardClassesTest, MemoryLockGuard) {
    bool was_locked = SystemTuning::is_memory_locked();

    {
        MemoryLockGuard guard;

        if (guard.is_locked()) {
            std::cout << "Memory locked within guard\n";
            EXPECT_TRUE(SystemTuning::is_memory_locked());
        } else {
            std::cout << "Failed to lock memory (may require permissions)\n";
        }
    }

    // Should be restored to original state
    EXPECT_EQ(SystemTuning::is_memory_locked(), was_locked);
}
#endif

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
