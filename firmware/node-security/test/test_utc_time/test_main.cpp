#include <unity.h>

#include <climits>
#include <cstdint>

#include "command_guard.h"
#include "utc_time.h"

using node_sec::ClockContext;
using node_sec::CommandAckStatus;
using node_sec::CommandEnvelope;
using node_sec::CommandGuard;
using node_sec::CommandGuardDecision;
using node_sec::CommandGuardReason;
using node_sec::UtcParseError;
using node_sec::UtcParseResult;
using node_sec::UtcParser;

void setUp(void) {}
void tearDown(void) {}

void test_known_utc_epochs_before_and_after_1970(void) {
    UtcParseResult res = UtcParser::parseIso8601UtcMs("1970-01-01T00:00:00Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::NONE), static_cast<uint8_t>(res.error));
    TEST_ASSERT_EQUAL_INT64(0LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("1969-12-31T23:59:59Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(-1000LL, res.epoch_ms);

    // Exact timestamps from contracts/examples/node-b-command-security-mode.json & node-b-command-servo.json.
    res = UtcParser::parseIso8601UtcMs("2026-11-16T01:40:00Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(1794793200000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("2026-11-16T01:40:30Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(1794793230000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("2026-11-16T01:42:00Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(1794793320000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("2026-11-16T01:42:30Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(1794793350000LL, res.epoch_ms);

    // Year 2038+ and 2106+ boundaries prove 64-bit math.
    res = UtcParser::parseIso8601UtcMs("2038-01-19T03:14:08Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(2147483648000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("2106-02-07T06:28:16Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(4294967296000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("0001-01-01T00:00:00Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(-62135596800000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("9999-12-31T23:59:59Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(253402300799000LL, res.epoch_ms);
}

void test_leap_year_and_calendar_validation(void) {
    UtcParseResult res = UtcParser::parseIso8601UtcMs("2000-02-29T12:34:56Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(951827696000LL, res.epoch_ms);

    res = UtcParser::parseIso8601UtcMs("2024-02-29T23:59:59Z");
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL_INT64(1709251199000LL, res.epoch_ms);

    const char* bad_days[] = {
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
    for (size_t i = 0; i < sizeof(bad_days) / sizeof(bad_days[0]); ++i) {
        res = UtcParser::parseIso8601UtcMs(bad_days[i]);
        TEST_ASSERT_FALSE(res.ok);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_DAY_OF_MONTH),
                                static_cast<uint8_t>(res.error));
    }

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

void test_invalid_time_components_and_output_preserved_on_error(void) {
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(UtcParseError::INVALID_HOUR),
        static_cast<uint8_t>(UtcParser::parseIso8601UtcMs("2026-11-16T24:00:00Z").error));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(UtcParseError::INVALID_MINUTE),
        static_cast<uint8_t>(UtcParser::parseIso8601UtcMs("2026-11-16T23:60:00Z").error));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(UtcParseError::INVALID_SECOND),
        static_cast<uint8_t>(UtcParser::parseIso8601UtcMs("2026-11-16T23:59:60Z").error));

    int64_t sentinel = -1234567LL;
    UtcParseError err = UtcParseError::NONE;
    TEST_ASSERT_FALSE(UtcParser::tryParseIso8601UtcMs("2026-02-29T00:00:00Z", &sentinel, &err));
    TEST_ASSERT_EQUAL_INT64(-1234567LL, sentinel);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::INVALID_DAY_OF_MONTH),
                            static_cast<uint8_t>(err));
}

void test_timezone_offsets_fractional_seconds_and_malformed_inputs_rejected(void) {
    const char* tz_offsets[] = {
        "2026-11-16T01:40:00+07:00",
        "2026-11-16T01:40:00-05:00",
        "2026-11-16T01:40:00+00:00",
    };
    for (size_t i = 0; i < sizeof(tz_offsets) / sizeof(tz_offsets[0]); ++i) {
        const UtcParseResult res = UtcParser::parseIso8601UtcMs(tz_offsets[i]);
        TEST_ASSERT_FALSE(res.ok);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::UNSUPPORTED_TIMEZONE_OFFSET),
                                static_cast<uint8_t>(res.error));
    }

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(UtcParseError::INVALID_UTC_DESIGNATOR),
        static_cast<uint8_t>(UtcParser::parseIso8601UtcMs("2026-11-16T01:40:00z").error));

    // Fractional seconds (e.g. Node.js Date.toISOString() .000Z) must be rejected by strict v1 contract.
    const char* invalid_inputs[] = {
        "",
        "2026-11-16T01:40:00",
        "2026-11-16T01:40:00.000Z",
        " 2026-11-16T01:40:00Z",
        "2026-11-16T01:40:00Z ",
        "2026-11-16t01:40:00Z",
        "2026-11-16 01:40:00Z",
        "2026/11/16T01:40:00Z",
        "2026-11-16T01-40-00Z",
        "2026-1A-16T01:40:00Z",
    };
    for (size_t i = 0; i < sizeof(invalid_inputs) / sizeof(invalid_inputs[0]); ++i) {
        const UtcParseResult res = UtcParser::parseIso8601UtcMs(invalid_inputs[i]);
        TEST_ASSERT_FALSE(res.ok);
    }

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(UtcParseError::NULL_INPUT),
                            static_cast<uint8_t>(UtcParser::parseIso8601UtcMs(nullptr).error));
}

void test_int64_overflow_guards(void) {
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(UtcParseError::OVERFLOW),
        static_cast<uint8_t>(UtcParser::civilUtcToEpochMs(300000000LL, 1U, 1U, 0U, 0U, 0U).error));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(UtcParseError::OVERFLOW),
        static_cast<uint8_t>(UtcParser::civilUtcToEpochMs(-300000000LL, 1U, 1U, 0U, 0U, 0U).error));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(UtcParseError::OVERFLOW),
        static_cast<uint8_t>(UtcParser::civilUtcToEpochMs(LLONG_MAX, 1U, 1U, 0U, 0U, 0U).error));

    const UtcParseResult max_ok = UtcParser::civilUtcToEpochMs(290000000LL, 12U, 31U, 23U, 59U, 59U);
    TEST_ASSERT_TRUE(max_ok.ok);
    TEST_ASSERT_TRUE(max_ok.epoch_ms > 0LL);
}

void test_utc_parser_integrates_with_node_b_command_guard(void) {
    CommandGuard guard;
    CommandEnvelope cmd;
    cmd.schema_version = 1;
    cmd.device_id = "node-b";
    cmd.command_id = "cmd-nodeb-20261116-0001";
    cmd.has_issued_at = UtcParser::tryParseIso8601UtcMs("2026-11-16T01:40:00Z", &cmd.issued_at_epoch_ms);
    cmd.has_expires_at = UtcParser::tryParseIso8601UtcMs("2026-11-16T01:40:30Z", &cmd.expires_at_epoch_ms);
    cmd.has_ttl_ms = true;
    cmd.ttl_ms = 30000LL;
    cmd.require_time_sync = true;

    TEST_ASSERT_TRUE(cmd.has_issued_at);
    TEST_ASSERT_TRUE(cmd.has_expires_at);

    ClockContext clk;
    clk.time_synced = true;
    TEST_ASSERT_TRUE(UtcParser::tryParseIso8601UtcMs("2026-11-16T01:40:30Z", &clk.now_utc_epoch_ms));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(guard.validate(cmd, clk).decision));

    TEST_ASSERT_TRUE(UtcParser::tryParseIso8601UtcMs("2026-11-16T01:40:31Z", &clk.now_utc_epoch_ms));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::EXPIRED),
                            static_cast<uint8_t>(guard.validate(cmd, clk).ack_status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::COMMAND_EXPIRED_UTC),
                            static_cast<uint8_t>(guard.validate(cmd, clk).reason));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_known_utc_epochs_before_and_after_1970);
    RUN_TEST(test_leap_year_and_calendar_validation);
    RUN_TEST(test_invalid_time_components_and_output_preserved_on_error);
    RUN_TEST(test_timezone_offsets_fractional_seconds_and_malformed_inputs_rejected);
    RUN_TEST(test_int64_overflow_guards);
    RUN_TEST(test_utc_parser_integrates_with_node_b_command_guard);
    return UNITY_END();
}
