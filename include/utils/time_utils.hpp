/**
 * @file time_utils.hpp
 * @brief High-precision time utilities for low-latency applications
 *
 * Provides nanosecond-precision timestamps, RDTSC support for x86_64,
 * and ISO 8601 timestamp parsing/formatting. Designed for minimal
 * overhead in hot paths.
 *
 * @author Market Data Handler Team
 * @date 2024-11-24
 */

#pragma once

#include <cstdint>
#include <chrono>
#include <string>

#ifdef __x86_64__
#include <x86intrin.h>
#endif

namespace marketdata {

/**
 * @brief Get current timestamp in nanoseconds
 *
 * Uses std::chrono::high_resolution_clock for maximum precision.
 * Suitable for most timing needs in the application.
 *
 * @return uint64_t Nanoseconds since epoch
 *
 * @code
 * uint64_t start = get_timestamp_ns();
 * do_work();
 * uint64_t elapsed = get_timestamp_ns() - start;
 * @endcode
 *
 * @note Precision depends on system clock implementation
 * @see get_timestamp_us()
 * @see get_rdtsc() for ultra-low latency on x86_64
 */
inline uint64_t get_timestamp_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

/**
 * @brief Get current timestamp in microseconds
 *
 * @return uint64_t Microseconds since epoch
 *
 * @see get_timestamp_ns()
 */
inline uint64_t get_timestamp_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

/**
 * @brief Get current timestamp in milliseconds
 *
 * @return uint64_t Milliseconds since epoch
 *
 * @see get_timestamp_ns()
 */
inline uint64_t get_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

#ifdef __x86_64__
/**
 * @brief Get CPU cycle counter using RDTSC
 *
 * Reads the Time Stamp Counter (TSC) register on x86_64 processors.
 * Provides the lowest-latency timestamp possible (~20-30 cycles).
 *
 * @return uint64_t CPU cycle count
 *
 * @warning Not serializing - may be reordered by CPU
 * @warning Requires calibration to convert to nanoseconds
 * @note Only available on x86_64 architecture
 * @see get_rdtscp() for serializing version
 *
 * @code
 * uint64_t start = get_rdtsc();
 * // Hot path code
 * uint64_t cycles = get_rdtsc() - start;
 * @endcode
 */
inline uint64_t get_rdtsc() {
    return __rdtsc();
}

/**
 * @brief Get CPU cycle counter using serializing RDTSCP
 *
 * Similar to RDTSC but with serializing behavior - waits for all
 * previous instructions to complete before reading the counter.
 * More accurate but slightly slower than RDTSC.
 *
 * @return uint64_t CPU cycle count
 *
 * @note Only available on x86_64 architecture
 * @see get_rdtsc()
 */
inline uint64_t get_rdtscp() {
    unsigned int aux;
    return __rdtscp(&aux);
}
#endif

/**
 * @brief RAII timer for measuring code block execution time
 *
 * Automatically measures elapsed time from construction to destruction.
 * Useful for profiling and latency tracking.
 *
 * @code
 * uint64_t duration_ns = 0;
 * {
 *     ScopedTimer timer(duration_ns);
 *     expensive_operation();
 * }  // duration_ns now contains elapsed time
 * std::cout << "Operation took " << duration_ns << " ns\n";
 * @endcode
 */
class ScopedTimer {
public:
    /**
     * @brief Construct timer and start measuring
     *
     * @param duration_ns Reference to variable that will receive elapsed time
     */
    explicit ScopedTimer(uint64_t& duration_ns)
        : start_(get_timestamp_ns())
        , duration_ref_(duration_ns) {}

    /**
     * @brief Destroy timer and store elapsed time
     *
     * Automatically calculates and stores elapsed time when going out of scope.
     */
    ~ScopedTimer() {
        duration_ref_ = get_timestamp_ns() - start_;
    }

    /**
     * @brief Get elapsed time without destroying timer
     *
     * Allows checking elapsed time mid-execution without stopping the timer.
     *
     * @return uint64_t Nanoseconds elapsed since construction
     */
    uint64_t elapsed_ns() const {
        return get_timestamp_ns() - start_;
    }

private:
    uint64_t start_;           ///< Start timestamp
    uint64_t& duration_ref_;   ///< Reference to output variable
};

/**
 * @brief Parse ISO 8601 timestamp to nanoseconds
 *
 * Converts an ISO 8601 formatted timestamp string to nanoseconds since epoch.
 * Supports formats like: "2024-01-15T10:30:45.123456789Z"
 *
 * @param iso_timestamp ISO 8601 formatted timestamp string
 * @return uint64_t Nanoseconds since epoch
 *
 * @code
 * uint64_t ns = parse_iso8601_to_nanos("2024-01-15T10:30:45.123Z");
 * @endcode
 *
 * @see nanos_to_iso8601()
 */
uint64_t parse_iso8601_to_nanos(const std::string& iso_timestamp);

/**
 * @brief Convert nanoseconds to ISO 8601 string
 *
 * Converts nanoseconds since epoch to an ISO 8601 formatted string.
 * Output format: "YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ"
 *
 * @param nanos Nanoseconds since epoch
 * @return std::string ISO 8601 formatted timestamp
 *
 * @code
 * std::string iso = nanos_to_iso8601(get_timestamp_ns());
 * // Returns: "2024-11-24T15:30:45.123456789Z"
 * @endcode
 *
 * @see parse_iso8601_to_nanos()
 */
std::string nanos_to_iso8601(uint64_t nanos);

} // namespace marketdata
