#include <unity.h>

#include <climits>
#include <cstdint>

#include "command_guard.h"
#include "utc_time.h"

using node_env::ClockContext;
using node_env::CommandAckStatus;
using node_env::CommandEnvelope;
using node_env::CommandGuard;
using node_env::CommandGuardDecision;
using node_env::CommandGuardReason;
using node_env::UtcParseError;
using node_env::UtcParseResult;
using node_env::UtcParser;

void setUp(void) {}
void tearDown(void) {}

void test_known_utc_epochs_and_contract_command_timestamps(void) {
    UtcParseResult res = UtcParser::parseIso8601UtcMs("1970-01-01T00:00:00Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::NONE), static_cast<uint8_t>(res.error));
    TEST_ASSERT_EQUAL_INT64(0LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("1969-12-31T23:59:59Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(-1000LL, res.epoch_ms);

    // Exact timestamps from contracts/examples/node-a-command-lamp.json.
    res = UtcParser::parseIso8601UtcMs("2026-11-16T01:40:10Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(1794793210000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("2026-11-16T01:40:40Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(1794793240000LL, res.epoch_ms);

    // Year 2038+ and 2106+ boundaries prove 64-bit math (no signed/unsigned 32-bit overflow).
    res = UtcParser::parseIso8601UtcMs("2038-01-19T03:14:07Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(2147483647000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("2038-01-19T03:14:08Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(2147483648000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("2106-02-07T06:28:16Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(4294967296000LL, res.epoch_ms);

    // Proleptic calendar extremes.
    res = UtcParser::parseIso8601UtcMs("0001-01-01T00:00:00Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(-62135596800000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("9999-12-31T23:59:59Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(253402300799000LL, res.epoch_ms);
}

void test_leap_year_and_calendar_month_day_validation(void) {
    // 2000 (divisible by 400) and 2024 (divisible by 4, not 100) are leap years.
    UtcParseResult res = UtcParser::parseIso8601UtcMs("2000-02-29T12:34:56Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(951827696000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("2024-02-29T23:59:59Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(1709251199000LL, res.epoch_ms);

    // 1900, 2100 (divisible by 100, not 400) and 2026 must reject Feb 29.
    const char* invalid_feb29[] = {
        "1900-02-29T00:00:00Z",
        "2100-02-29T00:00:00Z",
        "2026-02-29T00:00:00Z",
        "2024-02-30T00:00:00Z",
        "2026-04-31T00:00:00Z",
        "2026-06-31T00:00:00Z",
        "2026-09-31T00:00:00Z",
        "2026-11-31T00:00:00Z",
        "2026-01-32T00:00:00Z",
        "2026-03-00T00:00:00Z",
    };
    for (size_t i = 0; i < sizeof(invalid_feb29) / sizeof(invalid_feb29[0]); ++i) {
        res = UtcParser::parseIso8601UtcMs(invalid_feb29[i]);
        TEST_ASSERT_FALSE(res.ok);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_DAY_OF_MONTH),
                                static_cast<uint8_t>(res.error));
    }

    // Invalid year 0000 and months 00, 13.
    res = UtcParser::parseIso8601UtcMs("0000-01-01T00:00:00Z");
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_YEAR),
                            static_cast<uint8_t>(res.error));

    res = UtcParser::parseIso8601UtcMs("2026-00-10T00:00:00Z");
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_MONTH),
                            static_cast<uint8_t>(res.error));

    res = UtcParser::parseIso8601UtcMs("2026-13-10T00:00:00Z");
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_MONTH),
                            static_cast<uint8_t>(res.error));
}

void test_time_field_boundaries_and_try_parse_preserves_output_on_error(void) {
    UtcParseResult res = UtcParser::parseIso8601UtcMs("2026-11-16T24:00:00Z");
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_HOUR),
                            static_cast<uint8_t>(res.error));

    res = UtcParser::parseIso8601UtcMs("2026-11-16T23:60:00Z");
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_MINUTE),
                            static_cast<uint8_t>(res.error));

    res = UtcParser::parseIso8601UtcMs("2026-11-16T23:59:60Z");
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_SECOND),
                            static_cast<uint8_t>(res.error));

    // Sentinel epoch_ms must remain untouched when parsing fails (no default timestamp fabricated).
    int64_t sentinel = -999999LL;
    UtcParseError err = UtcParseError::NONE;
    TEST_ASSERT_FALSE(UtcParser::tryParseIso8601UtcMs("invalid-ts", &sentinel, &err));
    TEST_ASSERT_EQUAL_INT64(-999999LL, sentinel);
    TEST_ASSERT_NOT_EQUAL(static_cast<uint8_t>(UtcParseError::NONE), static_cast<uint8_t>(err));
}

void test_timezone_offsets_and_format_errors_rejected(void) {
    // Non-UTC offsets (+07:00, -05:00, +00:00) must be rejected cleanly.
    const char* tz_offsets[] = {
        "2026-11-16T01:40:10+07:00",
        "2026-11-16T01:40:10-05:00",
        "2026-11-16T01:40:10+00:00",
    };
    for (size_t i = 0; i < sizeof(tz_offsets) / sizeof(tz_offsets[0]); ++i) {
        const UtcParseResult res = UtcParser::parseIso8601UtcMs(tz_offsets[i]);
        TEST_ASSERT_FALSE(res.ok);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::UNSUPPORTED_TIMEZONE_OFFSET),
                                static_cast<uint8_t>(res.error));
    }

    // Lowercase 'z' or wrong suffix at index 19.
    UtcParseResult res = UtcParser::parseIso8601UtcMs("2026-11-16T01:40:10z");
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_UTC_DESIGNATOR),
                            static_cast<uint8_t>(res.error));

    // Missing Z, fractional seconds, trailing spaces, lowercase 't', space separator, bad delimiters, null.
    const char* bad_formats[] = {
        "",
        "2026-11-16T01:40:10",
        "2026-11-16T01:40:10.000Z",
        " 2026-11-16T01:40:10Z",
        "2026-11-16T01:40:10Z ",
        "2026-11-16t01:40:10Z",
        "2026-11-16 01:40:10Z",
        "2026/11/16T01:40:10Z",
        "2026-11-16T01-40-10Z",
        "2026-1A-16T01:40:10Z",
    };
    for (size_t i = 0; i < sizeof(bad_formats) / sizeof(bad_formats[0]); ++i) {
        res = UtcParser::parseIso8601UtcMs(bad_formats[i]);
        TEST_ASSERT_FALSE(res.ok);
    }

    res = UtcParser::parseIso8601UtcMs(nullptr);
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::NULL_INPUT),
                            static_cast<uint8_t>(res.error));
}

void test_int64_overflow_guards_in_civil_conversion(void) {
    // Huge positive and negative years must trigger OVERFLOW cleanly without signed UB.
    UtcParseResult res = UtcParser::civilUtcToEpochMs(300000000LL, 1U, 1U, 0U, 0U, 0U);
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::OVERFLOW),
                            static_cast<uint8_t>(res.error));

    res = UtcParser::civilUtcToEpochMs(-300000000LL, 1U, 1U, 0U, 0U, 0U);
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::OVERFLOW),
                            static_cast<uint8_t>(res.error));

    res = UtcParser::civilUtcToEpochMs(LLONG_MAX, 1U, 1U, 0U, 0U, 0U);
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::OVERFLOW),
                            static_cast<uint8_t>(res.error));

    // Exact boundary near max int64 representable days:
    // LLONG_MAX / 86400000 = 106751991167 days -> year ~ 292277024 (caught by kMaxSafeCivilYear).
    // Verify 290000000-12-31T23:59:59Z succeeds cleanly inside int64_t range.
    res = UtcParser::civilUtcToEpochMs(290000000LL, 12U, 31U, 23U, 59U, 59U);
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_TRUE(res.epoch_ms > 0LL);
}

void test_utc_parser_integrates_with_command_guard_without_mutating_guard(void) {
    CommandGuard guard;
    CommandEnvelope cmd;
    cmd.schema_version = 1;
    cmd.device_id = "node-a";
    cmd.command_id = "cmd-nodea-20261116-0001";
    cmd.has_issued_at = UtcParser::tryParseIso8601UtcMs("2026-11-16T01:40:10Z", &cmd.issued_at_epoch_ms);
    cmd.has_expires_at = UtcParser::tryParseIso8601UtcMs("2026-11-16T01:40:40Z", &cmd.expires_at_epoch_ms);
    cmd.has_ttl_ms = true;
    cmd.ttl_ms = 30000LL;
    cmd.require_time_sync = true;

    TEST_ASSERT_TRUE(cmd.has_issued_at);
    TEST_ASSERT_TRUE(cmd.has_expires_at);

    ClockContext clk;
    clk.time_synced = true;
    TEST_ASSERT_TRUE(UtcParser::tryParseIso8601UtcMs("2026-11-16T01:40:40Z", &clk.now_utc_epoch_ms));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(guard.validate(cmd, clk).decision));

    TEST_ASSERT_TRUE(UtcParser::tryParseIso8601UtcMs("2026-11-16T01:40:41Z", &clk.now_utc_epoch_ms));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::EXPIRED),
                            static_cast<uint8_t>(guard.validate(cmd, clk).ack_status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::COMMAND_EXPIRED_UTC),
                            static_cast<uint8_t>(guard.validate(cmd, clk).reason));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_known_utc_epochs_and_contract_command_timestamps);
    RUN_TEST(test_leap_year_and_calendar_month_day_validation);
    RUN_TEST(test_time_field_boundaries_and_try_parse_preserves_output_on_error);
    RUN_TEST(test_timezone_offsets_and_format_errors_rejected);
    RUN_TEST(test_int64_overflow_guards_in_civil_conversion);
    RUN_TEST(test_utc_parser_integrates_with_command_guard_without_mutating_guard);
    return UNITY_END();
}
