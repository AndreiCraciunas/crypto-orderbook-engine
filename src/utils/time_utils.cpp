#include "utils/time_utils.hpp"
#include <iomanip>
#include <sstream>
#include <ctime>

namespace marketdata {

uint64_t parse_iso8601_to_nanos(const std::string& iso_timestamp) {
    // Parse ISO 8601 format: 2024-01-15T10:30:45.123456789Z
    std::tm tm = {};
    std::istringstream ss(iso_timestamp);

    // Parse date and time
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

    // Convert to time_t
    std::time_t time = std::mktime(&tm);
    uint64_t nanos = static_cast<uint64_t>(time) * 1000000000ULL;

    // Parse fractional seconds if present
    if (ss.peek() == '.') {
        ss.get(); // consume '.'
        std::string fraction;
        while (std::isdigit(ss.peek())) {
            fraction += static_cast<char>(ss.get());
        }

        // Pad or trim to 9 digits (nanoseconds)
        if (fraction.length() < 9) {
            fraction.append(9 - fraction.length(), '0');
        } else if (fraction.length() > 9) {
            fraction = fraction.substr(0, 9);
        }

        nanos += std::stoull(fraction);
    }

    return nanos;
}

std::string nanos_to_iso8601(uint64_t nanos) {
    // Convert to seconds and nanosecond remainder
    uint64_t seconds = nanos / 1000000000ULL;
    uint64_t nano_remainder = nanos % 1000000000ULL;

    // Convert to time_t
    std::time_t time = static_cast<std::time_t>(seconds);
    std::tm* tm = std::gmtime(&time);

    // Format ISO 8601 string
    std::ostringstream ss;
    ss << std::put_time(tm, "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(9) << nano_remainder << 'Z';

    return ss.str();
}

} // namespace marketdata
