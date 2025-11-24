/**
 * @file configuration_manager.cpp
 * @brief Configuration manager implementation
 */

#include "config/configuration_manager.hpp"
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <fstream>
#include <cstdlib>

namespace marketdata {

ConfigurationManager::ConfigurationManager() {
    config_ = create_default();
}

bool ConfigurationManager::load(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        std::ifstream file(filename);
        if (!file.is_open()) {
            spdlog::error("ConfigurationManager: Failed to open file {}", filename);
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

        if (!parse_yaml(content)) {
            return false;
        }

        filename_ = filename;
        spdlog::info("ConfigurationManager: Loaded configuration from {}", filename);

        // Trigger callback
        if (change_callback_) {
            change_callback_(config_);
        }

        return true;

    } catch (const std::exception& e) {
        spdlog::error("ConfigurationManager: Exception loading {}: {}", filename, e.what());
        return false;
    }
}

bool ConfigurationManager::load_from_string(const std::string& yaml_content) {
    std::lock_guard<std::mutex> lock(mutex_);
    return parse_yaml(yaml_content);
}

bool ConfigurationManager::save(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        YAML::Emitter out;
        out << YAML::BeginMap;

        // Exchanges
        out << YAML::Key << "exchanges";
        out << YAML::Value << YAML::BeginMap;

        for (const auto& [exchange, cfg] : config_.exchanges) {
            std::string ex_name;
            switch (exchange) {
                case Exchange::BINANCE: ex_name = "binance"; break;
                case Exchange::COINBASE: ex_name = "coinbase"; break;
                case Exchange::KRAKEN: ex_name = "kraken"; break;
            }

            out << YAML::Key << ex_name;
            out << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "enabled" << YAML::Value << cfg.enabled;
            out << YAML::Key << "websocket_url" << YAML::Value << cfg.websocket_url;
            out << YAML::Key << "rest_url" << YAML::Value << cfg.rest_url;
            out << YAML::Key << "use_testnet" << YAML::Value << cfg.use_testnet;
            out << YAML::Key << "depth" << YAML::Value << cfg.depth;
            out << YAML::Key << "rate_limit_per_sec" << YAML::Value << cfg.rate_limit_per_sec;
            out << YAML::Key << "symbols" << YAML::Value << cfg.symbols;
            out << YAML::EndMap;
        }

        out << YAML::EndMap;

        // Server
        out << YAML::Key << "server";
        out << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "host" << YAML::Value << config_.server.host;
        out << YAML::Key << "websocket_port" << YAML::Value << config_.server.websocket_port;
        out << YAML::Key << "rest_port" << YAML::Value << config_.server.rest_port;
        out << YAML::EndMap;

        // Performance
        out << YAML::Key << "performance";
        out << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "enable_cpu_pinning" << YAML::Value << config_.performance.enable_cpu_pinning;
        out << YAML::Key << "cpu_cores" << YAML::Value << config_.performance.cpu_cores;
        out << YAML::EndMap;

        // Logging
        out << YAML::Key << "logging";
        out << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "level" << YAML::Value << config_.logging.level;
        out << YAML::Key << "async" << YAML::Value << config_.logging.async;
        out << YAML::EndMap;

        out << YAML::EndMap;

        std::ofstream file(filename);
        file << out.c_str();

        spdlog::info("ConfigurationManager: Saved configuration to {}", filename);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("ConfigurationManager: Failed to save {}: {}", filename, e.what());
        return false;
    }
}

SystemConfig ConfigurationManager::get_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

ExchangeConfig ConfigurationManager::get_exchange_config(Exchange exchange) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = config_.exchanges.find(exchange);
    if (it != config_.exchanges.end()) {
        return it->second;
    }

    // Return default
    ExchangeConfig cfg;
    cfg.exchange = exchange;
    return cfg;
}

std::vector<Exchange> ConfigurationManager::get_enabled_exchanges() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Exchange> enabled;
    for (const auto& [exchange, cfg] : config_.exchanges) {
        if (cfg.enabled) {
            enabled.push_back(exchange);
        }
    }

    return enabled;
}

std::vector<std::string> ConfigurationManager::validate() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> errors;

    // Validate exchanges
    for (const auto& [exchange, cfg] : config_.exchanges) {
        if (cfg.enabled) {
            if (cfg.websocket_url.empty()) {
                errors.push_back("Exchange websocket_url is empty");
            }
            if (cfg.symbols.empty()) {
                errors.push_back("Exchange has no symbols configured");
            }
            if (cfg.rate_limit_per_sec <= 0.0) {
                errors.push_back("Exchange rate_limit_per_sec must be > 0");
            }
        }
    }

    // Validate server
    if (config_.server.websocket_port == 0) {
        errors.push_back("Server websocket_port must be set");
    }
    if (config_.server.rest_port == 0) {
        errors.push_back("Server rest_port must be set");
    }
    if (config_.server.websocket_port == config_.server.rest_port) {
        errors.push_back("WebSocket and REST ports must be different");
    }

    return errors;
}

void ConfigurationManager::set_change_callback(ConfigChangeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    change_callback_ = callback;
}

bool ConfigurationManager::reload() {
    if (filename_.empty()) {
        spdlog::warn("ConfigurationManager: No filename set, cannot reload");
        return false;
    }

    return load(filename_);
}

SystemConfig ConfigurationManager::create_default() {
    SystemConfig config;

    // Binance
    ExchangeConfig binance;
    binance.exchange = Exchange::BINANCE;
    binance.enabled = true;
    binance.websocket_url = "wss://stream.binance.com:9443/ws";
    binance.rest_url = "https://api.binance.com";
    binance.rate_limit_per_sec = 20.0;
    binance.symbols = {"btcusdt", "ethusdt"};
    config.exchanges[Exchange::BINANCE] = binance;

    // Coinbase
    ExchangeConfig coinbase;
    coinbase.exchange = Exchange::COINBASE;
    coinbase.enabled = true;
    coinbase.websocket_url = "wss://ws-feed.exchange.coinbase.com";
    coinbase.rest_url = "https://api.exchange.coinbase.com";
    coinbase.rate_limit_per_sec = 30.0;
    coinbase.symbols = {"BTC-USD", "ETH-USD"};
    config.exchanges[Exchange::COINBASE] = coinbase;

    // Kraken
    ExchangeConfig kraken;
    kraken.exchange = Exchange::KRAKEN;
    kraken.enabled = true;
    kraken.websocket_url = "wss://ws.kraken.com";
    kraken.rest_url = "https://api.kraken.com";
    kraken.rate_limit_per_sec = 15.0;
    kraken.depth = 10;
    kraken.symbols = {"XBT/USD", "ETH/USD"};
    config.exchanges[Exchange::KRAKEN] = kraken;

    return config;
}

bool ConfigurationManager::create_example(const std::string& filename) {
    auto config = create_default();
    ConfigurationManager manager;
    manager.config_ = config;
    return manager.save(filename);
}

bool ConfigurationManager::parse_yaml(const std::string& yaml_content) {
    try {
        YAML::Node root = YAML::Load(yaml_content);

        // Parse exchanges
        if (root["exchanges"]) {
            auto exchanges = root["exchanges"];

            // Binance
            if (exchanges["binance"]) {
                auto node = exchanges["binance"];
                ExchangeConfig cfg;
                cfg.exchange = Exchange::BINANCE;
                cfg.enabled = node["enabled"].as<bool>(true);
                cfg.websocket_url = substitute_env_vars(node["websocket_url"].as<std::string>(""));
                cfg.rest_url = substitute_env_vars(node["rest_url"].as<std::string>(""));
                cfg.use_testnet = node["use_testnet"].as<bool>(false);
                cfg.depth = node["depth"].as<uint32_t>(10);
                cfg.rate_limit_per_sec = node["rate_limit_per_sec"].as<double>(20.0);

                if (node["symbols"]) {
                    cfg.symbols = node["symbols"].as<std::vector<std::string>>();
                }

                config_.exchanges[Exchange::BINANCE] = cfg;
            }

            // Coinbase
            if (exchanges["coinbase"]) {
                auto node = exchanges["coinbase"];
                ExchangeConfig cfg;
                cfg.exchange = Exchange::COINBASE;
                cfg.enabled = node["enabled"].as<bool>(true);
                cfg.websocket_url = substitute_env_vars(node["websocket_url"].as<std::string>(""));
                cfg.rest_url = substitute_env_vars(node["rest_url"].as<std::string>(""));
                cfg.use_testnet = node["use_testnet"].as<bool>(false);
                cfg.rate_limit_per_sec = node["rate_limit_per_sec"].as<double>(30.0);

                if (node["symbols"]) {
                    cfg.symbols = node["symbols"].as<std::vector<std::string>>();
                }

                config_.exchanges[Exchange::COINBASE] = cfg;
            }

            // Kraken
            if (exchanges["kraken"]) {
                auto node = exchanges["kraken"];
                ExchangeConfig cfg;
                cfg.exchange = Exchange::KRAKEN;
                cfg.enabled = node["enabled"].as<bool>(true);
                cfg.websocket_url = substitute_env_vars(node["websocket_url"].as<std::string>(""));
                cfg.rest_url = substitute_env_vars(node["rest_url"].as<std::string>(""));
                cfg.use_testnet = node["use_testnet"].as<bool>(false);
                cfg.depth = node["depth"].as<uint32_t>(10);
                cfg.rate_limit_per_sec = node["rate_limit_per_sec"].as<double>(15.0);

                if (node["symbols"]) {
                    cfg.symbols = node["symbols"].as<std::vector<std::string>>();
                }

                config_.exchanges[Exchange::KRAKEN] = cfg;
            }
        }

        // Parse server
        if (root["server"]) {
            auto server = root["server"];
            config_.server.host = server["host"].as<std::string>("0.0.0.0");
            config_.server.websocket_port = server["websocket_port"].as<uint16_t>(8080);
            config_.server.rest_port = server["rest_port"].as<uint16_t>(8081);
            config_.server.max_connections = server["max_connections"].as<size_t>(1000);
        }

        // Parse performance
        if (root["performance"]) {
            auto perf = root["performance"];
            config_.performance.enable_cpu_pinning = perf["enable_cpu_pinning"].as<bool>(false);
            if (perf["cpu_cores"]) {
                config_.performance.cpu_cores = perf["cpu_cores"].as<std::vector<int>>();
            }
            config_.performance.enable_huge_pages = perf["enable_huge_pages"].as<bool>(false);
            config_.performance.enable_numa = perf["enable_numa"].as<bool>(false);
        }

        // Parse logging
        if (root["logging"]) {
            auto log = root["logging"];
            config_.logging.level = log["level"].as<std::string>("info");
            config_.logging.pattern = log["pattern"].as<std::string>("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
            config_.logging.async = log["async"].as<bool>(true);
            config_.logging.log_file = log["log_file"].as<std::string>("");
        }

        // Parse recording
        if (root["recording"]) {
            auto rec = root["recording"];
            config_.recording.enabled = rec["enabled"].as<bool>(false);
            config_.recording.output_dir = rec["output_dir"].as<std::string>("./data");
            config_.recording.format = rec["format"].as<std::string>("binary");
        }

        // Parse arbitrage
        if (root["arbitrage"]) {
            auto arb = root["arbitrage"];
            config_.arbitrage.enabled = arb["enabled"].as<bool>(true);
            config_.arbitrage.min_profit_percent = arb["min_profit_percent"].as<double>(0.1);
        }

        return true;

    } catch (const YAML::Exception& e) {
        spdlog::error("ConfigurationManager: YAML parse error: {}", e.what());
        return false;
    } catch (const std::exception& e) {
        spdlog::error("ConfigurationManager: Parse error: {}", e.what());
        return false;
    }
}

std::string ConfigurationManager::substitute_env_vars(const std::string& value) const {
    std::string result = value;
    size_t pos = 0;

    while ((pos = result.find("${", pos)) != std::string::npos) {
        size_t end = result.find("}", pos);
        if (end == std::string::npos) {
            break;
        }

        std::string var_name = result.substr(pos + 2, end - pos - 2);
        const char* env_value = std::getenv(var_name.c_str());

        if (env_value) {
            result.replace(pos, end - pos + 1, env_value);
            pos += strlen(env_value);
        } else {
            pos = end + 1;
        }
    }

    return result;
}

} // namespace marketdata
