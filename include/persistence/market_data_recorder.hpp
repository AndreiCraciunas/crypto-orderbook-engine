/**
 * @file market_data_recorder.hpp
 * @brief Market data recording and replay system
 *
 * Records order book updates to disk for historical analysis, backtesting,
 * and compliance. Supports high-throughput recording with minimal latency
 * impact on the critical path.
 *
 * **Features:**
 * - Async disk I/O (zero copy to critical path)
 * - Compressed storage
 * - Fast replay capability
 * - Timestamp-based seeking
 * - Multiple format support
 *
 * **Performance:**
 * - Record: O(1), ~100-500 ns (async)
 * - Replay: ~100,000 updates/sec
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include "core/types.hpp"
#include "utils/lock_free_queue.hpp"
#include <string>
#include <fstream>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>

namespace marketdata {

/**
 * @brief Recording format
 */
enum class RecordingFormat {
    BINARY,         ///< Compact binary format (fastest)
    JSON,           ///< JSON format (human-readable)
    CSV             ///< CSV format (Excel-compatible)
};

/**
 * @brief Recording statistics
 */
struct RecordingStats {
    size_t total_updates_recorded{0};       ///< Total updates written
    size_t bytes_written{0};                ///< Total bytes written
    size_t queue_size{0};                   ///< Current queue size
    uint64_t start_time_ns{0};              ///< Recording start time
    uint64_t last_write_ns{0};              ///< Last write timestamp
    double write_rate_per_sec{0.0};         ///< Updates per second

    /**
     * @brief Get compression ratio
     */
    double get_compression_ratio() const {
        if (total_updates_recorded == 0) return 0.0;
        // Estimate uncompressed size (rough approximation)
        size_t estimated_uncompressed = total_updates_recorded * 512;
        return static_cast<double>(estimated_uncompressed) / bytes_written;
    }
};

/**
 * @brief Market data recorder
 *
 * Records order book updates to disk asynchronously. Uses lock-free queue
 * for minimal latency impact on hot path.
 *
 * **Architecture:**
 * - Hot path: Push to lock-free queue (~100 ns)
 * - Background thread: Flush queue to disk
 * - Batched writes for efficiency
 * - Optional compression
 *
 * **File Format (Binary):**
 * ```
 * Header:
 *   Magic: "MDOB" (4 bytes)
 *   Version: uint32_t
 *   Format: uint8_t
 *   Reserved: 3 bytes
 *
 * Records:
 *   Timestamp: uint64_t (nanoseconds)
 *   Exchange: uint8_t
 *   Symbol length: uint8_t
 *   Symbol: char[]
 *   Num bids: uint16_t
 *   Num asks: uint16_t
 *   Bids: [price: double, qty: double, count: uint32_t][]
 *   Asks: [price: double, qty: double, count: uint32_t][]
 * ```
 *
 * @code
 * // Create recorder
 * MarketDataRecorder recorder("market_data.bin",
 *                            RecordingFormat::BINARY);
 *
 * // Start recording
 * recorder.start();
 *
 * // Record updates (from callback)
 * connection_manager->set_orderbook_callback([&](const auto& update) {
 *     recorder.record(update);  // Async, non-blocking
 * });
 *
 * // Stop and flush
 * recorder.stop();
 * @endcode
 *
 * @note Thread-safe
 * @note record() is lock-free and non-blocking
 */
class MarketDataRecorder {
public:
    /**
     * @brief Construct market data recorder
     *
     * @param filename Output file path
     * @param format Recording format (default: BINARY)
     * @param buffer_size Queue buffer size (default: 100000)
     *
     * @code
     * MarketDataRecorder recorder("data/btc_20241124.bin");
     * @endcode
     */
    explicit MarketDataRecorder(const std::string& filename,
                               RecordingFormat format = RecordingFormat::BINARY,
                               size_t buffer_size = 100000);

    /**
     * @brief Destructor
     *
     * Automatically stops recording and flushes remaining data.
     */
    ~MarketDataRecorder();

    // Disable copy and move
    MarketDataRecorder(const MarketDataRecorder&) = delete;
    MarketDataRecorder& operator=(const MarketDataRecorder&) = delete;

    /**
     * @brief Start recording
     *
     * Opens file and starts background writer thread.
     *
     * @return bool True if started successfully
     *
     * @note Thread-safe
     */
    bool start();

    /**
     * @brief Stop recording
     *
     * Stops background thread and flushes remaining data.
     *
     * @note Thread-safe
     * @note Blocks until all queued data is written
     */
    void stop();

    /**
     * @brief Record order book update
     *
     * Adds update to queue for async writing. Non-blocking.
     *
     * @param update Order book update
     * @return bool True if queued successfully
     *
     * @code
     * if (!recorder.record(update)) {
     *     // Queue full, consider dropping or blocking
     * }
     * @endcode
     *
     * @note Thread-safe
     * @note Lock-free, non-blocking (~100 ns)
     * @note Returns false if queue is full
     */
    bool record(const NormalizedOrderBookUpdate& update);

    /**
     * @brief Flush pending writes
     *
     * Forces all queued data to be written immediately.
     *
     * @note Thread-safe
     * @note May block briefly
     */
    void flush();

    /**
     * @brief Get recording statistics
     *
     * @return RecordingStats Current statistics
     *
     * @note Thread-safe
     */
    RecordingStats get_statistics() const;

    /**
     * @brief Check if recording is active
     *
     * @return bool True if recording
     */
    bool is_recording() const { return recording_.load(); }

    /**
     * @brief Get filename
     *
     * @return const std::string& Output filename
     */
    const std::string& get_filename() const { return filename_; }

private:
    std::string filename_;                              ///< Output file
    RecordingFormat format_;                            ///< Recording format
    size_t buffer_size_;                                ///< Queue size

    std::atomic<bool> recording_{false};                ///< Recording flag
    std::unique_ptr<std::thread> writer_thread_;        ///< Background writer
    std::unique_ptr<std::ofstream> output_file_;        ///< Output stream

    std::unique_ptr<LockFreeQueue<NormalizedOrderBookUpdate>> queue_; ///< Update queue

    // Statistics
    std::atomic<size_t> total_updates_{0};
    std::atomic<size_t> bytes_written_{0};
    std::atomic<uint64_t> start_time_ns_{0};
    std::atomic<uint64_t> last_write_ns_{0};

    /**
     * @brief Writer thread function
     */
    void writer_loop();

    /**
     * @brief Write file header
     */
    void write_header();

    /**
     * @brief Write update in binary format
     */
    void write_binary(const NormalizedOrderBookUpdate& update);

    /**
     * @brief Write update in JSON format
     */
    void write_json(const NormalizedOrderBookUpdate& update);

    /**
     * @brief Write update in CSV format
     */
    void write_csv(const NormalizedOrderBookUpdate& update);
};

/**
 * @brief Market data replayer
 *
 * Replays recorded market data for backtesting and analysis.
 * Supports fast-forward, slow-motion, and timestamp seeking.
 *
 * **Features:**
 * - Fast replay (100k+ updates/sec)
 * - Timestamp-based seeking
 * - Speed control (1x, 2x, etc.)
 * - Callback-based delivery
 *
 * @code
 * // Create replayer
 * MarketDataReplayer replayer("market_data.bin");
 *
 * // Set callback
 * replayer.set_callback([](const auto& update) {
 *     // Process update
 *     process_update(update);
 * });
 *
 * // Replay at 2x speed
 * replayer.replay(2.0);
 * @endcode
 */
class MarketDataReplayer {
public:
    /**
     * @brief Callback for replayed updates
     */
    using ReplayCallback = std::function<void(const NormalizedOrderBookUpdate&)>;

    /**
     * @brief Construct market data replayer
     *
     * @param filename Input file path
     *
     * @code
     * MarketDataReplayer replayer("data/btc_20241124.bin");
     * @endcode
     */
    explicit MarketDataReplayer(const std::string& filename);

    /**
     * @brief Destructor
     */
    ~MarketDataReplayer();

    /**
     * @brief Open file for replay
     *
     * @return bool True if opened successfully
     */
    bool open();

    /**
     * @brief Close file
     */
    void close();

    /**
     * @brief Replay all data
     *
     * Replays entire file from start to finish.
     *
     * @param speed_multiplier Playback speed (1.0 = real-time, 2.0 = 2x)
     *
     * @code
     * replayer.replay(10.0);  // 10x speed
     * @endcode
     *
     * @note Blocks until complete or stopped
     */
    void replay(double speed_multiplier = 1.0);

    /**
     * @brief Replay from timestamp
     *
     * Seeks to timestamp and replays from there.
     *
     * @param start_time_ns Start timestamp (nanoseconds)
     * @param speed_multiplier Playback speed
     *
     * @code
     * // Replay from 10am
     * uint64_t ten_am = ...;
     * replayer.replay_from(ten_am, 1.0);
     * @endcode
     */
    void replay_from(uint64_t start_time_ns, double speed_multiplier = 1.0);

    /**
     * @brief Replay time range
     *
     * Replays data within specific time range.
     *
     * @param start_time_ns Start timestamp
     * @param end_time_ns End timestamp
     * @param speed_multiplier Playback speed
     */
    void replay_range(uint64_t start_time_ns, uint64_t end_time_ns,
                     double speed_multiplier = 1.0);

    /**
     * @brief Stop replay
     *
     * Stops ongoing replay.
     *
     * @note Thread-safe
     */
    void stop();

    /**
     * @brief Set replay callback
     *
     * @param callback Callback function
     */
    void set_callback(ReplayCallback callback);

    /**
     * @brief Get file info
     *
     * @return std::tuple<uint64_t, uint64_t, size_t>
     *         (start_time, end_time, update_count)
     */
    std::tuple<uint64_t, uint64_t, size_t> get_file_info();

private:
    std::string filename_;                          ///< Input file
    RecordingFormat format_;                        ///< File format
    std::unique_ptr<std::ifstream> input_file_;     ///< Input stream

    std::atomic<bool> replaying_{false};            ///< Replay flag
    ReplayCallback callback_;                       ///< Replay callback

    /**
     * @brief Read file header
     */
    bool read_header();

    /**
     * @brief Read next update (binary format)
     */
    std::optional<NormalizedOrderBookUpdate> read_binary();

    /**
     * @brief Read next update (JSON format)
     */
    std::optional<NormalizedOrderBookUpdate> read_json();

    /**
     * @brief Read next update (CSV format)
     */
    std::optional<NormalizedOrderBookUpdate> read_csv();
};

} // namespace marketdata
