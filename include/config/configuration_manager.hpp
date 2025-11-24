/**
 * @file configuration_manager.hpp
 * @brief YAML-based configuration management system
 *
 * Provides centralized configuration management for the entire trading system.
 * Supports YAML configuration files with validation, hot-reloading, and
 * environment variable substitution.
 *
 * **Features:**
 * - YAML configuration parsing
 * - Type-safe configuration access
 * - Default values
 * - Configuration validation
 * - Hot-reloading support
 * - Environment variable substitution
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "core/types.hpp"
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>

namespace marketdata {

/**
 * @brief Exchange configuration
 */
struct ExchangeConfig {
    Exchange exchange;                      ///< Exchange identifier
    bool enabled{true};                     ///< Enable/disable exchange
    std::string websocket_url;              ///< WebSocket endpoint
    std::string rest_url;                   ///< REST API endpoint
    bool use_testnet{false};                ///< Use testnet/sandbox
    uint32_t depth{10};                     ///< Order book depth
    uint32_t reconnect_delay_ms{5000};      ///< Reconnect delay
    uint32_t max_reconnect_attempts{10};    ///< Max reconnect attempts
    double rate_limit_per_sec{20.0};        ///< Rate limit
    std::vector<std::string> symbols;       ///< Symbols to subscribe
};

/**
 * @brief Server configuration
 */
struct ServerConfig {
    std::string host{"0.0.0.0"};            ///< Bind host
    uint16_t websocket_port{8080};          ///< WebSocket port
    uint16_t rest_port{8081};               ///< REST API port
    size_t max_connections{1000};           ///< Max concurrent connections
    uint32_t heartbeat_interval_ms{30000};  ///< Heartbeat interval
};

/**
 * @brief Performance configuration
 */
struct PerformanceConfig {
    bool enable_cpu_pinning{false};         ///< Enable CPU affinity
    std::vector<int> cpu_cores;             ///< CPU cores for pinning
    bool enable_huge_pages{false};          ///< Enable huge pages
    bool enable_numa{false};                ///< Enable NUMA awareness
    size_t memory_pool_size{100000};        ///< Memory pool size
    size_t queue_size{100000};              ///< Lock-free queue size
};

/**
 * @brief Logging configuration
 */
struct LoggingConfig {
    std::string level{"info"};              ///< Log level
    std::string pattern{"[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v"};  ///< Log pattern
    bool async{true};                       ///< Async logging
    std::string log_file;                   ///< Log file path
    size_t max_file_size{10485760};         ///< Max file size (10MB)
    size_t max_files{5};                    ///< Max rotating files
};

/**
 * @brief Recording configuration
 */
struct RecordingConfig {
    bool enabled{false};                    ///< Enable recording
    std::string output_dir{"./data"};       ///< Output directory
    std::string format{"binary"};           ///< Format (binary/json/csv)
    size_t buffer_size{100000};             ///< Buffer size
};

/**
 * @brief Analytics configuration
 */
struct AnalyticsConfig {
    uint64_t vwap_window_ns{60'000'000'000ULL};     ///< VWAP window (60s)
    uint64_t twap_window_ns{300'000'000'000ULL};    ///< TWAP window (5min)
    uint64_t imbalance_window_ns{10'000'000'000ULL};///< Imbalance window (10s)
    uint64_t spread_window_ns{60'000'000'000ULL};   ///< Spread window (60s)
};

/**
 * @brief Arbitrage configuration
 */
struct ArbitrageConfig {
    bool enabled{true};                     ///< Enable arbitrage detection
    double min_profit_percent{0.1};         ///< Min profit threshold
    size_t history_size{1000};              ///< History size
};

/**
 * @brief Complete system configuration
 */
struct SystemConfig {
    std::map<Exchange, ExchangeConfig> exchanges;   ///< Exchange configs
    ServerConfig server;                            ///< Server config
    PerformanceConfig performance;                  ///< Performance config
    LoggingConfig logging;                          ///< Logging config
    RecordingConfig recording;                      ///< Recording config
    AnalyticsConfig analytics;                      ///< Analytics config
    ArbitrageConfig arbitrage;                      ///< Arbitrage config
};

/**
 * @brief Configuration manager
 *
 * Manages system-wide configuration from YAML files.
 * Provides type-safe access to configuration values.
 *
 * **Configuration File Format:**
 * ```yaml
 * exchanges:
 *   binance:
 *     enabled: true
 *     websocket_url: "wss://stream.binance.com:9443/ws"
 *     rest_url: "https://api.binance.com"
 *     symbols: ["btcusdt", "ethusdt"]
 *     rate_limit_per_sec: 20.0
 *
 *   coinbase:
 *     enabled: true
 *     websocket_url: "wss://ws-feed.exchange.coinbase.com"
 *     symbols: ["BTC-USD", "ETH-USD"]
 *
 * server:
 *   websocket_port: 8080
 *   rest_port: 8081
 *
 * performance:
 *   enable_cpu_pinning: true
 *   cpu_cores: [2, 3, 4, 5]
 *
 * logging:
 *   level: "info"
 *   async: true
 * ```
 *
 * @code
 * // Load configuration
 * ConfigurationManager config;
 * config.load("config.yaml");
 *
 * // Access configuration
 * auto& binance_config = config.get_exchange_config(Exchange::BINANCE);
 * std::cout << "Binance enabled: " << binance_config.enabled << "\n";
 *
 * // Get server port
 * uint16_t port = config.get_config().server.websocket_port;
 * @endcode
 *
 * @note Thread-safe
 */
class ConfigurationManager {
public:
    /**
     * @brief Callback for configuration changes
     */
    using ConfigChangeCallback = std::function<void(const SystemConfig&)>;

    /**
     * @brief Construct configuration manager
     */
    ConfigurationManager();

    /**
     * @brief Load configuration from YAML file
     *
     * @param filename YAML configuration file path
     * @return bool True if loaded successfully
     *
     * @code
     * ConfigurationManager config;
     * if (!config.load("config.yaml")) {
     *     std::cerr << "Failed to load config\n";
     * }
     * @endcode
     *
     * @note Thread-safe
     */
    bool load(const std::string& filename);

    /**
     * @brief Load configuration from string
     *
     * @param yaml_content YAML content
     * @return bool True if loaded successfully
     *
     * @note Thread-safe
     */
    bool load_from_string(const std::string& yaml_content);

    /**
     * @brief Save configuration to file
     *
     * @param filename Output file path
     * @return bool True if saved successfully
     *
     * @note Thread-safe
     */
    bool save(const std::string& filename) const;

    /**
     * @brief Get complete system configuration
     *
     * @return const SystemConfig& Configuration reference
     *
     * @note Thread-safe (returns copy)
     */
    SystemConfig get_config() const;

    /**
     * @brief Get exchange configuration
     *
     * @param exchange Exchange identifier
     * @return ExchangeConfig Exchange configuration
     *
     * @code
     * auto binance = config.get_exchange_config(Exchange::BINANCE);
     * for (const auto& symbol : binance.symbols) {
     *     std::cout << "Symbol: " << symbol << "\n";
     * }
     * @endcode
     *
     * @note Thread-safe
     * @note Returns default if not found
     */
    ExchangeConfig get_exchange_config(Exchange exchange) const;

    /**
     * @brief Get enabled exchanges
     *
     * @return std::vector<Exchange> List of enabled exchanges
     *
     * @note Thread-safe
     */
    std::vector<Exchange> get_enabled_exchanges() const;

    /**
     * @brief Validate configuration
     *
     * Checks for invalid values and required fields.
     *
     * @return std::vector<std::string> List of validation errors
     *
     * @code
     * auto errors = config.validate();
     * if (!errors.empty()) {
     *     for (const auto& error : errors) {
     *         std::cerr << "Config error: " << error << "\n";
     *     }
     * }
     * @endcode
     */
    std::vector<std::string> validate() const;

    /**
     * @brief Set configuration change callback
     *
     * Callback is invoked when configuration is reloaded.
     *
     * @param callback Change callback function
     *
     * @note Thread-safe
     */
    void set_change_callback(ConfigChangeCallback callback);

    /**
     * @brief Reload configuration from original file
     *
     * @return bool True if reloaded successfully
     *
     * @note Thread-safe
     * @note Triggers change callback if set
     */
    bool reload();

    /**
     * @brief Get configuration file path
     *
     * @return const std::string& File path
     */
    const std::string& get_filename() const { return filename_; }

    /**
     * @brief Create default configuration
     *
     * @return SystemConfig Default configuration
     *
     * @note Static method
     */
    static SystemConfig create_default();

    /**
     * @brief Create example configuration file
     *
     * @param filename Output file path
     * @return bool True if created successfully
     *
     * @code
     * ConfigurationManager::create_example("config.example.yaml");
     * @endcode
     */
    static bool create_example(const std::string& filename);

private:
    mutable std::mutex mutex_;              ///< Thread safety
    SystemConfig config_;                   ///< Current configuration
    std::string filename_;                  ///< Configuration file path
    ConfigChangeCallback change_callback_;  ///< Change callback

    /**
     * @brief Parse YAML content
     *
     * @param yaml_content YAML string
     * @return bool True if parsed successfully
     */
    bool parse_yaml(const std::string& yaml_content);

    /**
     * @brief Substitute environment variables
     *
     * @param value String with ${VAR} placeholders
     * @return std::string Substituted string
     */
    std::string substitute_env_vars(const std::string& value) const;
};

} // namespace marketdata
