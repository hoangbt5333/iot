#pragma once

#include <cstddef>
#include <cstdint>

namespace node_env {

enum class UtcParseError : uint8_t {
    NONE = 0,
    NULL_INPUT = 1,
    INVALID_LENGTH = 2,
    INVALID_FORMAT = 3,
    UNSUPPORTED_TIMEZONE_OFFSET = 4,
    INVALID_UTC_DESIGNATOR = 5,
    INVALID_YEAR = 6,
    INVALID_MONTH = 7,
    INVALID_DAY_OF_MONTH = 8,
    INVALID_HOUR = 9,
    INVALID_MINUTE = 10,
    INVALID_SECOND = 11,
    OVERFLOW = 12
};

struct UtcParseResult {
    bool ok = false;
    UtcParseError error = UtcParseError::NONE;
    int64_t epoch_ms = 0;
};

// Pure C++ strict ISO-8601 UTC ("YYYY-MM-DDTHH:MM:SSZ") parser independent of Arduino.h and mktime().
class UtcParser {
public:
    static constexpr size_t kIso8601UtcLength = 20U;
    static constexpr int64_t kMinIsoYear = 1LL;
    static constexpr int64_t kMaxIsoYear = 9999LL;

    // Parses strictly "YYYY-MM-DDTHH:MM:SSZ". On failure, ok=false and epoch_ms stays 0.
    static UtcParseResult parseIso8601UtcMs(const char* timestamp);

    // Writes out_epoch_ms ONLY when parsing succeeds; never supplies a fallback timestamp on error.
    static bool tryParseIso8601UtcMs(const char* timestamp,
                                     int64_t* out_epoch_ms,
                                     UtcParseError* out_error = nullptr);

    // Converts proleptic Gregorian UTC date-time components to epoch ms with int64_t overflow guards.
    static UtcParseResult civilUtcToEpochMs(int64_t year,
                                            uint8_t month,
                                            uint8_t day,
                                            uint8_t hour,
                                            uint8_t minute,
                                            uint8_t second);

    static bool isLeapYear(int64_t year);
    static uint8_t daysInMonth(int64_t year, uint8_t month);
    static const char* expectedFormat();
    static const char* errorToString(UtcParseError error);
};

}  // namespace node_env
