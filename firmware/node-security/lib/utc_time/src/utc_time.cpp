#include "utc_time.h"

#include <climits>

namespace node_sec {

namespace {

constexpr int64_t kMsPerSecond = 1000LL;
constexpr int64_t kSecondsPerMinute = 60LL;
constexpr int64_t kSecondsPerHour = 3600LL;
constexpr int64_t kMsPerDay = 86400000LL;
// Safe proleptic Gregorian year bound preventing signed overflow in era * 146097.
constexpr int64_t kMaxSafeCivilYear = 290000000LL;
constexpr size_t kMaxInspectLen = 40U;

bool isAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

bool parseDigits2(const char* ptr, uint8_t* out_val) {
    if (!isAsciiDigit(ptr[0]) || !isAsciiDigit(ptr[1])) {
        return false;
    }
    *out_val = static_cast<uint8_t>((ptr[0] - '0') * 10 + (ptr[1] - '0'));
    return true;
}

bool parseDigits4(const char* ptr, int64_t* out_val) {
    int64_t acc = 0;
    for (size_t i = 0; i < 4U; ++i) {
        if (!isAsciiDigit(ptr[i])) {
            return false;
        }
        acc = acc * 10LL + static_cast<int64_t>(ptr[i] - '0');
    }
    *out_val = acc;
    return true;
}

bool hasDateTimePrefixSeparators(const char* ts) {
    return ts[4] == '-' && ts[7] == '-' && ts[10] == 'T' && ts[13] == ':' && ts[16] == ':';
}

// Howard Hinnant's proleptic Gregorian days-since-1970-01-01 algorithm (strictly UTC, no tzset/mktime).
int64_t daysFromCivil(int64_t year, uint8_t month, uint8_t day) {
    year -= (month <= 2U) ? 1LL : 0LL;
    const int64_t era = (year >= 0LL ? year : year - 399LL) / 400LL;
    const int64_t yoe = year - era * 400LL;
    const int64_t m_adj = static_cast<int64_t>(month) + (month > 2U ? -3LL : 9LL);
    const int64_t doy = (153LL * m_adj + 2LL) / 5LL + static_cast<int64_t>(day) - 1LL;
    const int64_t doe = yoe * 365LL + yoe / 4LL - yoe / 100LL + doy;
    return era * 146097LL + doe - 719468LL;
}

UtcParseResult makeError(UtcParseError err) {
    UtcParseResult res;
    res.ok = false;
    res.error = err;
    res.epoch_ms = 0;
    return res;
}

}  // namespace

UtcParseResult UtcParser::parseIso8601UtcMs(const char* timestamp) {
    if (timestamp == nullptr) {
        return makeError(UtcParseError::NULL_INPUT);
    }

    size_t len = 0U;
    while (len <= kMaxInspectLen && timestamp[len] != '\0') {
        ++len;
    }

    if (len >= kIso8601UtcLength && hasDateTimePrefixSeparators(timestamp)) {
        if (timestamp[19] == '+' || timestamp[19] == '-') {
            return makeError(UtcParseError::UNSUPPORTED_TIMEZONE_OFFSET);
        }
        if (len == kIso8601UtcLength && timestamp[19] != 'Z') {
            return makeError(UtcParseError::INVALID_UTC_DESIGNATOR);
        }
    }

    if (len != kIso8601UtcLength) {
        return makeError(UtcParseError::INVALID_LENGTH);
    }

    if (!hasDateTimePrefixSeparators(timestamp) || timestamp[19] != 'Z') {
        return makeError(UtcParseError::INVALID_FORMAT);
    }

    int64_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;

    if (!parseDigits4(timestamp + 0, &year) ||
        !parseDigits2(timestamp + 5, &month) ||
        !parseDigits2(timestamp + 8, &day) ||
        !parseDigits2(timestamp + 11, &hour) ||
        !parseDigits2(timestamp + 14, &minute) ||
        !parseDigits2(timestamp + 17, &second)) {
        return makeError(UtcParseError::INVALID_FORMAT);
    }

    if (year < kMinIsoYear || year > kMaxIsoYear) {
        return makeError(UtcParseError::INVALID_YEAR);
    }

    return civilUtcToEpochMs(year, month, day, hour, minute, second);
}

bool UtcParser::tryParseIso8601UtcMs(const char* timestamp,
                                      int64_t* out_epoch_ms,
                                      UtcParseError* out_error) {
    const UtcParseResult res = parseIso8601UtcMs(timestamp);
    if (out_error != nullptr) {
        *out_error = res.error;
    }
    if (!res.ok) {
        return false;
    }
    if (out_epoch_ms != nullptr) {
        *out_epoch_ms = res.epoch_ms;
    }
    return true;
}

UtcParseResult UtcParser::civilUtcToEpochMs(int64_t year,
                                            uint8_t month,
                                            uint8_t day,
                                            uint8_t hour,
                                            uint8_t minute,
                                            uint8_t second) {
    if (year > kMaxSafeCivilYear || year < -kMaxSafeCivilYear) {
        return makeError(UtcParseError::OVERFLOW);
    }
    if (month < 1U || month > 12U) {
        return makeError(UtcParseError::INVALID_MONTH);
    }
    const uint8_t max_day = daysInMonth(year, month);
    if (day < 1U || day > max_day) {
        return makeError(UtcParseError::INVALID_DAY_OF_MONTH);
    }
    if (hour > 23U) {
        return makeError(UtcParseError::INVALID_HOUR);
    }
    if (minute > 59U) {
        return makeError(UtcParseError::INVALID_MINUTE);
    }
    if (second > 59U) {
        return makeError(UtcParseError::INVALID_SECOND);
    }

    const int64_t days = daysFromCivil(year, month, day);
    const int64_t tod_ms =
        (static_cast<int64_t>(hour) * kSecondsPerHour +
         static_cast<int64_t>(minute) * kSecondsPerMinute +
         static_cast<int64_t>(second)) *
        kMsPerSecond;

    // Explicit int64_t overflow/underflow guards before days * kMsPerDay + tod_ms.
    if (days > (LLONG_MAX - tod_ms) / kMsPerDay) {
        return makeError(UtcParseError::OVERFLOW);
    }
    if (days < LLONG_MIN / kMsPerDay) {
        return makeError(UtcParseError::OVERFLOW);
    }

    UtcParseResult res;
    res.ok = true;
    res.error = UtcParseError::NONE;
    res.epoch_ms = days * kMsPerDay + tod_ms;
    return res;
}

bool UtcParser::isLeapYear(int64_t year) {
    return (year % 4LL == 0LL) && ((year % 100LL != 0LL) || (year % 400LL == 0LL));
}

uint8_t UtcParser::daysInMonth(int64_t year, uint8_t month) {
    static constexpr uint8_t kDaysPerMonth[12] = {31U, 28U, 31U, 30U, 31U, 30U,
                                                  31U, 31U, 30U, 31U, 30U, 31U};
    if (month < 1U || month > 12U) {
        return 0U;
    }
    if (month == 2U && isLeapYear(year)) {
        return 29U;
    }
    return kDaysPerMonth[month - 1U];
}

const char* UtcParser::expectedFormat() {
    return "YYYY-MM-DDTHH:MM:SSZ";
}

const char* UtcParser::errorToString(UtcParseError error) {
    switch (error) {
        case UtcParseError::NONE:
            return "none";
        case UtcParseError::NULL_INPUT:
            return "null_input";
        case UtcParseError::INVALID_LENGTH:
            return "invalid_length";
        case UtcParseError::INVALID_FORMAT:
            return "invalid_format";
        case UtcParseError::UNSUPPORTED_TIMEZONE_OFFSET:
            return "unsupported_timezone_offset";
        case UtcParseError::INVALID_UTC_DESIGNATOR:
            return "invalid_utc_designator";
        case UtcParseError::INVALID_YEAR:
            return "invalid_year";
        case UtcParseError::INVALID_MONTH:
            return "invalid_month";
        case UtcParseError::INVALID_DAY_OF_MONTH:
            return "invalid_day_of_month";
        case UtcParseError::INVALID_HOUR:
            return "invalid_hour";
        case UtcParseError::INVALID_MINUTE:
            return "invalid_minute";
        case UtcParseError::INVALID_SECOND:
            return "invalid_second";
        case UtcParseError::OVERFLOW:
            return "overflow";
        default:
            return "unknown";
    }
}

}  // namespace node_sec
