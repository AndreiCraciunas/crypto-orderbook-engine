/**
 * @file market_data_recorder.cpp
 * @brief Market data recorder implementation
 */

#include "persistence/market_data_recorder.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <cstring>
#include <iomanip>

namespace marketdata {

// File format constants
constexpr uint32_t MAGIC_NUMBER = 0x424F444D;  // "MDOB" in little-endian
constexpr uint32_t FORMAT_VERSION = 1;

MarketDataRecorder::MarketDataRecorder(const std::string& filename,
                                       RecordingFormat format,
                                       size_t buffer_size)
    : filename_(filename)
    , format_(format)
    , buffer_size_(buffer_size)
    , queue_(std::make_unique<LockFreeQueue<NormalizedOrderBookUpdate>>()) {

    spdlog::info("MarketDataRecorder: Created for file {} (format: {}, buffer: {})",
                filename_, static_cast<int>(format_), buffer_size_);
}

MarketDataRecorder::~MarketDataRecorder() {
    if (recording_.load()) {
        stop();
    }
}

bool MarketDataRecorder::start() {
    if (recording_.exchange(true)) {
        spdlog::warn("MarketDataRecorder: Already recording");
        return false;
    }

    // Open output file
    std::ios_base::openmode mode = std::ios::out;
    if (format_ == RecordingFormat::BINARY) {
        mode |= std::ios::binary;
    }

    output_file_ = std::make_unique<std::ofstream>(filename_, mode);
    if (!output_file_->is_open()) {
        spdlog::error("MarketDataRecorder: Failed to open file {}", filename_);
        recording_.store(false);
        return false;
    }

    // Write header
    write_header();

    // Reset statistics
    total_updates_.store(0);
    bytes_written_.store(0);
    start_time_ns_.store(get_timestamp_ns());
    last_write_ns_.store(0);

    // Start writer thread
    writer_thread_ = std::make_unique<std::thread>([this]() {
        writer_loop();
    });

    spdlog::info("MarketDataRecorder: Recording started");
    return true;
}

void MarketDataRecorder::stop() {
    if (!recording_.exchange(false)) {
        spdlog::warn("MarketDataRecorder: Not recording");
        return;
    }

    // Wait for writer thread
    if (writer_thread_ && writer_thread_->joinable()) {
        writer_thread_->join();
    }

    // Close file
    if (output_file_ && output_file_->is_open()) {
        output_file_->flush();
        output_file_->close();
    }

    auto stats = get_statistics();
    spdlog::info("MarketDataRecorder: Recording stopped - {} updates, {} bytes written",
                stats.total_updates_recorded, stats.bytes_written);
}

bool MarketDataRecorder::record(const NormalizedOrderBookUpdate& update) {
    if (!recording_.load()) {
        return false;
    }

    // Try to push to queue (non-blocking)
    queue_->push(update);
    return true;
}

void MarketDataRecorder::flush() {
    if (output_file_ && output_file_->is_open()) {
        output_file_->flush();
    }
}

RecordingStats MarketDataRecorder::get_statistics() const {
    RecordingStats stats;
    stats.total_updates_recorded = total_updates_.load();
    stats.bytes_written = bytes_written_.load();
    stats.start_time_ns = start_time_ns_.load();
    stats.last_write_ns = last_write_ns_.load();

    // Estimate queue size (approximate)
    stats.queue_size = 0;  // LockFreeQueue doesn't expose size easily

    // Calculate write rate
    uint64_t now = get_timestamp_ns();
    uint64_t elapsed_ns = now - stats.start_time_ns;
    if (elapsed_ns > 0) {
        double elapsed_sec = static_cast<double>(elapsed_ns) / 1'000'000'000.0;
        stats.write_rate_per_sec = stats.total_updates_recorded / elapsed_sec;
    }

    return stats;
}

void MarketDataRecorder::writer_loop() {
    spdlog::debug("MarketDataRecorder: Writer thread started");

    size_t batch_count = 0;
    const size_t batch_flush_size = 100;  // Flush every 100 updates

    while (recording_.load()) {
        auto update = queue_->pop();

        if (update) {
            // Write update based on format
            switch (format_) {
                case RecordingFormat::BINARY:
                    write_binary(*update);
                    break;
                case RecordingFormat::JSON:
                    write_json(*update);
                    break;
                case RecordingFormat::CSV:
                    write_csv(*update);
                    break;
            }

            total_updates_.fetch_add(1);
            last_write_ns_.store(get_timestamp_ns());

            // Periodic flush
            batch_count++;
            if (batch_count >= batch_flush_size) {
                output_file_->flush();
                batch_count = 0;
            }
        } else {
            // No data available, sleep briefly
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // Final flush of remaining data
    while (auto update = queue_->pop()) {
        switch (format_) {
            case RecordingFormat::BINARY:
                write_binary(*update);
                break;
            case RecordingFormat::JSON:
                write_json(*update);
                break;
            case RecordingFormat::CSV:
                write_csv(*update);
                break;
        }
        total_updates_.fetch_add(1);
    }

    output_file_->flush();
    spdlog::debug("MarketDataRecorder: Writer thread stopped");
}

void MarketDataRecorder::write_header() {
    if (format_ == RecordingFormat::BINARY) {
        // Magic number
        uint32_t magic = MAGIC_NUMBER;
        output_file_->write(reinterpret_cast<const char*>(&magic), sizeof(magic));

        // Version
        uint32_t version = FORMAT_VERSION;
        output_file_->write(reinterpret_cast<const char*>(&version), sizeof(version));

        // Format
        uint8_t fmt = static_cast<uint8_t>(format_);
        output_file_->write(reinterpret_cast<const char*>(&fmt), sizeof(fmt));

        // Reserved (3 bytes)
        uint8_t reserved[3] = {0, 0, 0};
        output_file_->write(reinterpret_cast<const char*>(reserved), sizeof(reserved));

        bytes_written_.fetch_add(sizeof(magic) + sizeof(version) + sizeof(fmt) + sizeof(reserved));
    } else if (format_ == RecordingFormat::CSV) {
        // Write CSV header
        *output_file_ << "timestamp_ns,exchange,symbol,side,price,quantity,order_count\n";
    }
}

void MarketDataRecorder::write_binary(const NormalizedOrderBookUpdate& update) {
    size_t bytes_before = output_file_->tellp();

    // Timestamp
    output_file_->write(reinterpret_cast<const char*>(&update.timestamp), sizeof(update.timestamp));

    // Exchange
    uint8_t exchange = static_cast<uint8_t>(update.exchange);
    output_file_->write(reinterpret_cast<const char*>(&exchange), sizeof(exchange));

    // Symbol
    uint8_t symbol_len = static_cast<uint8_t>(update.symbol.size());
    output_file_->write(reinterpret_cast<const char*>(&symbol_len), sizeof(symbol_len));
    output_file_->write(update.symbol.data(), symbol_len);

    // Bids
    uint16_t num_bids = static_cast<uint16_t>(update.bids.size());
    output_file_->write(reinterpret_cast<const char*>(&num_bids), sizeof(num_bids));

    for (const auto& bid : update.bids) {
        output_file_->write(reinterpret_cast<const char*>(&bid.price), sizeof(bid.price));
        output_file_->write(reinterpret_cast<const char*>(&bid.quantity), sizeof(bid.quantity));
        output_file_->write(reinterpret_cast<const char*>(&bid.order_count), sizeof(bid.order_count));
    }

    // Asks
    uint16_t num_asks = static_cast<uint16_t>(update.asks.size());
    output_file_->write(reinterpret_cast<const char*>(&num_asks), sizeof(num_asks));

    for (const auto& ask : update.asks) {
        output_file_->write(reinterpret_cast<const char*>(&ask.price), sizeof(ask.price));
        output_file_->write(reinterpret_cast<const char*>(&ask.quantity), sizeof(ask.quantity));
        output_file_->write(reinterpret_cast<const char*>(&ask.order_count), sizeof(ask.order_count));
    }

    size_t bytes_after = output_file_->tellp();
    bytes_written_.fetch_add(bytes_after - bytes_before);
}

void MarketDataRecorder::write_json(const NormalizedOrderBookUpdate& update) {
    // Simple JSON format
    *output_file_ << "{"
                 << "\"timestamp\":" << update.timestamp << ","
                 << "\"exchange\":" << static_cast<int>(update.exchange) << ","
                 << "\"symbol\":\"" << update.symbol << "\","
                 << "\"bids\":[";

    for (size_t i = 0; i < update.bids.size(); i++) {
        if (i > 0) *output_file_ << ",";
        *output_file_ << "[" << std::fixed << std::setprecision(8)
                     << update.bids[i].price << ","
                     << update.bids[i].quantity << ","
                     << update.bids[i].order_count << "]";
    }

    *output_file_ << "],\"asks\":[";

    for (size_t i = 0; i < update.asks.size(); i++) {
        if (i > 0) *output_file_ << ",";
        *output_file_ << "[" << std::fixed << std::setprecision(8)
                     << update.asks[i].price << ","
                     << update.asks[i].quantity << ","
                     << update.asks[i].order_count << "]";
    }

    *output_file_ << "]}\n";

    bytes_written_.fetch_add(output_file_->tellp());
}

void MarketDataRecorder::write_csv(const NormalizedOrderBookUpdate& update) {
    // Write bids
    for (const auto& bid : update.bids) {
        *output_file_ << update.timestamp << ","
                     << static_cast<int>(update.exchange) << ","
                     << update.symbol << ","
                     << "bid,"
                     << std::fixed << std::setprecision(8) << bid.price << ","
                     << bid.quantity << ","
                     << bid.order_count << "\n";
    }

    // Write asks
    for (const auto& ask : update.asks) {
        *output_file_ << update.timestamp << ","
                     << static_cast<int>(update.exchange) << ","
                     << update.symbol << ","
                     << "ask,"
                     << std::fixed << std::setprecision(8) << ask.price << ","
                     << ask.quantity << ","
                     << ask.order_count << "\n";
    }

    bytes_written_.fetch_add(output_file_->tellp());
}

// MarketDataReplayer implementation

MarketDataReplayer::MarketDataReplayer(const std::string& filename)
    : filename_(filename) {
    spdlog::info("MarketDataReplayer: Created for file {}", filename_);
}

MarketDataReplayer::~MarketDataReplayer() {
    close();
}

bool MarketDataReplayer::open() {
    std::ios_base::openmode mode = std::ios::in;

    // Try binary first
    input_file_ = std::make_unique<std::ifstream>(filename_, mode | std::ios::binary);

    if (!input_file_->is_open()) {
        spdlog::error("MarketDataReplayer: Failed to open file {}", filename_);
        return false;
    }

    // Read and validate header
    if (!read_header()) {
        spdlog::error("MarketDataReplayer: Invalid file format");
        return false;
    }

    spdlog::info("MarketDataReplayer: File opened successfully");
    return true;
}

void MarketDataReplayer::close() {
    if (input_file_ && input_file_->is_open()) {
        input_file_->close();
    }
}

void MarketDataReplayer::replay(double speed_multiplier) {
    if (!input_file_ || !input_file_->is_open()) {
        spdlog::error("MarketDataReplayer: File not open");
        return;
    }

    replaying_.store(true);
    uint64_t last_timestamp = 0;
    uint64_t start_real_time = get_timestamp_ns();
    uint64_t start_data_time = 0;

    size_t count = 0;

    while (replaying_.load()) {
        std::optional<NormalizedOrderBookUpdate> update;

        // Read based on format
        switch (format_) {
            case RecordingFormat::BINARY:
                update = read_binary();
                break;
            case RecordingFormat::JSON:
                update = read_json();
                break;
            case RecordingFormat::CSV:
                update = read_csv();
                break;
        }

        if (!update) {
            break;  // End of file
        }

        if (start_data_time == 0) {
            start_data_time = update->timestamp;
        }

        // Calculate delay to maintain timing
        if (last_timestamp > 0 && speed_multiplier > 0.0) {
            uint64_t data_elapsed = update->timestamp - start_data_time;
            uint64_t real_elapsed = get_timestamp_ns() - start_real_time;
            uint64_t expected_elapsed = static_cast<uint64_t>(data_elapsed / speed_multiplier);

            if (expected_elapsed > real_elapsed) {
                uint64_t sleep_ns = expected_elapsed - real_elapsed;
                std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
            }
        }

        // Invoke callback
        if (callback_) {
            callback_(*update);
        }

        last_timestamp = update->timestamp;
        count++;
    }

    spdlog::info("MarketDataReplayer: Replayed {} updates", count);
    replaying_.store(false);
}

void MarketDataReplayer::replay_from(uint64_t start_time_ns, double speed_multiplier) {
    // Simple implementation: read until we find the timestamp
    // More efficient implementation would use index or binary search

    if (!input_file_ || !input_file_->is_open()) {
        spdlog::error("MarketDataReplayer: File not open");
        return;
    }

    // Seek to start of data (after header)
    input_file_->seekg(16);  // Skip header for binary format

    // Find starting position
    while (true) {
        auto update = read_binary();
        if (!update) {
            spdlog::warn("MarketDataReplayer: Start timestamp not found");
            return;
        }

        if (update->timestamp >= start_time_ns) {
            // Found it, rewind one record
            // (This is simplified - would need to track position properly)
            break;
        }
    }

    replay(speed_multiplier);
}

void MarketDataReplayer::replay_range(uint64_t start_time_ns, uint64_t end_time_ns,
                                     double speed_multiplier) {
    // Seek to start
    replay_from(start_time_ns, speed_multiplier);
    // TODO: Stop at end_time_ns
}

void MarketDataReplayer::stop() {
    replaying_.store(false);
}

void MarketDataReplayer::set_callback(ReplayCallback callback) {
    callback_ = callback;
}

std::tuple<uint64_t, uint64_t, size_t> MarketDataReplayer::get_file_info() {
    if (!input_file_ || !input_file_->is_open()) {
        return {0, 0, 0};
    }

    uint64_t start_time = 0;
    uint64_t end_time = 0;
    size_t count = 0;

    // Save current position
    auto pos = input_file_->tellg();

    // Read first update
    input_file_->seekg(16);  // Skip header
    if (auto update = read_binary()) {
        start_time = update->timestamp;
        count++;
    }

    // Scan entire file (inefficient but simple)
    while (auto update = read_binary()) {
        end_time = update->timestamp;
        count++;
    }

    // Restore position
    input_file_->clear();
    input_file_->seekg(pos);

    return {start_time, end_time, count};
}

bool MarketDataReplayer::read_header() {
    if (format_ == RecordingFormat::BINARY) {
        uint32_t magic;
        input_file_->read(reinterpret_cast<char*>(&magic), sizeof(magic));

        if (magic != MAGIC_NUMBER) {
            return false;
        }

        uint32_t version;
        input_file_->read(reinterpret_cast<char*>(&version), sizeof(version));

        uint8_t fmt;
        input_file_->read(reinterpret_cast<char*>(&fmt), sizeof(fmt));
        format_ = static_cast<RecordingFormat>(fmt);

        uint8_t reserved[3];
        input_file_->read(reinterpret_cast<char*>(reserved), sizeof(reserved));

        return true;
    }

    return true;  // JSON/CSV don't have binary headers
}

std::optional<NormalizedOrderBookUpdate> MarketDataReplayer::read_binary() {
    NormalizedOrderBookUpdate update;

    // Read timestamp
    input_file_->read(reinterpret_cast<char*>(&update.timestamp), sizeof(update.timestamp));
    if (input_file_->eof()) {
        return std::nullopt;
    }

    // Read exchange
    uint8_t exchange;
    input_file_->read(reinterpret_cast<char*>(&exchange), sizeof(exchange));
    update.exchange = static_cast<Exchange>(exchange);

    // Read symbol
    uint8_t symbol_len;
    input_file_->read(reinterpret_cast<char*>(&symbol_len), sizeof(symbol_len));

    char symbol_buf[256];
    input_file_->read(symbol_buf, symbol_len);
    update.symbol = std::string(symbol_buf, symbol_len);

    // Read bids
    uint16_t num_bids;
    input_file_->read(reinterpret_cast<char*>(&num_bids), sizeof(num_bids));

    update.bids.reserve(num_bids);
    for (uint16_t i = 0; i < num_bids; i++) {
        PriceLevelSnapshot level;
        input_file_->read(reinterpret_cast<char*>(&level.price), sizeof(level.price));
        input_file_->read(reinterpret_cast<char*>(&level.quantity), sizeof(level.quantity));
        input_file_->read(reinterpret_cast<char*>(&level.order_count), sizeof(level.order_count));
        update.bids.push_back(level);
    }

    // Read asks
    uint16_t num_asks;
    input_file_->read(reinterpret_cast<char*>(&num_asks), sizeof(num_asks));

    update.asks.reserve(num_asks);
    for (uint16_t i = 0; i < num_asks; i++) {
        PriceLevelSnapshot level;
        input_file_->read(reinterpret_cast<char*>(&level.price), sizeof(level.price));
        input_file_->read(reinterpret_cast<char*>(&level.quantity), sizeof(level.quantity));
        input_file_->read(reinterpret_cast<char*>(&level.order_count), sizeof(level.order_count));
        update.asks.push_back(level);
    }

    return update;
}

std::optional<NormalizedOrderBookUpdate> MarketDataReplayer::read_json() {
    // TODO: Implement JSON parsing
    return std::nullopt;
}

std::optional<NormalizedOrderBookUpdate> MarketDataReplayer::read_csv() {
    // TODO: Implement CSV parsing
    return std::nullopt;
}

} // namespace marketdata
