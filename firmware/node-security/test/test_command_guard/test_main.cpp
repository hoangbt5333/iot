#include <unity.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <utility>

#include "command_guard.h"
#include "node_security.h"

using node_sec::AdmissionResult;
using node_sec::ClockContext;
using node_sec::CommandAckStatus;
using node_sec::CommandAction;
using node_sec::CommandEnvelope;
using node_sec::CommandGuard;
using node_sec::CommandGuardConfig;
using node_sec::CommandGuardDecision;
using node_sec::CommandGuardReason;
using node_sec::CommandResult;
using node_sec::CommandStatus;
using node_sec::CommandTarget;
using node_sec::NodeBCommand;
using node_sec::SecurityController;
using node_sec::SecurityMode;
using node_sec::SwitchArbitrationMode;

namespace {

constexpr int64_t kIssuedUtcMs = 1794793200000LL;   // 2026-11-16T01:40:00Z
constexpr int64_t kExpiresUtcMs = 1794793230000LL;  // 2026-11-16T01:40:30Z

CommandEnvelope makeValidNodeBEnvelope(const char* command_id) {
    CommandEnvelope env;
    env.schema_version = 1;
    env.device_id = "node-b";
    env.command_id = command_id;
    env.has_issued_at = true;
    env.issued_at_epoch_ms = kIssuedUtcMs;
    env.has_expires_at = true;
    env.expires_at_epoch_ms = kExpiresUtcMs;
    env.has_ttl_ms = true;
    env.ttl_ms = 30000LL;
    env.require_time_sync = true;
    return env;
}

ClockContext makeSyncedClock(int64_t now_utc_ms) {
    ClockContext clk;
    clk.time_synced = true;
    clk.now_utc_epoch_ms = now_utc_ms;
    clk.now_uptime_ms = 20000ULL;
    clk.session_online = true;
    clk.session_online_uptime_ms = 5000ULL;
    clk.current_boot_id = "boot-b-0001-7c8d";
    return clk;
}

CommandGuard makeGuardFromScopedStackBuffer(const char* src_id,
                                          size_t dedup_capacity,
                                          int64_t max_ttl_ms,
                                          int64_t max_future_skew_ms,
                                          bool allow_unsynced_session_ttl) {
    char scoped_buf[CommandGuard::kMaxDeviceIdLen + 2U]{};
    const size_t len = std::strlen(src_id);
    if (len < sizeof(scoped_buf)) {
        std::memcpy(scoped_buf, src_id, len + 1U);
    }
    CommandGuardConfig cfg;
    cfg.expected_device_id = scoped_buf;
    cfg.dedup_capacity = dedup_capacity;
    cfg.max_ttl_ms = max_ttl_ms;
    cfg.max_future_skew_ms = max_future_skew_ms;
    cfg.allow_unsynced_session_ttl = allow_unsynced_session_ttl;

    CommandGuard guard(cfg);
    // Overwrite local buffer before leaving scope; guard must already hold its own copy.
    std::memset(scoped_buf, 'Z', sizeof(scoped_buf) - 1U);
    scoped_buf[sizeof(scoped_buf) - 1U] = '\0';
    return guard;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_valid_node_b_command_admitted_and_validate_does_not_mark_dedup(void) {
    CommandGuard guard;
    TEST_ASSERT_EQUAL_STRING("node-b", guard.expectedDeviceId());
    TEST_ASSERT_EQUAL_UINT32(24U, static_cast<uint32_t>(guard.dedupCapacity()));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));

    const CommandEnvelope cmd = makeValidNodeBEnvelope("cmd-nodeb-20261116-0001");
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    // Repeated validate() calls must NOT mark the command processed.
    AdmissionResult r1 = guard.validate(cmd, clk);
    AdmissionResult r2 = guard.validate(cmd, clk);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(r1.decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(r2.decision));
    TEST_ASSERT_FALSE(guard.isDuplicate(cmd.command_id));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));

    TEST_ASSERT_TRUE(guard.recordProcessed(cmd.command_id));
    TEST_ASSERT_TRUE(guard.isDuplicate(cmd.command_id));
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(guard.dedupCount()));

    AdmissionResult r3 = guard.validate(cmd, clk);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                            static_cast<uint8_t>(r3.decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::DUPLICATE),
                            static_cast<uint8_t>(r3.ack_status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::ALREADY_PROCESSED),
                            static_cast<uint8_t>(r3.reason));
}

void test_two_phase_security_apply_keeps_rejected_command_unmarked_and_preserves_policies(void) {
    CommandGuard guard;
    SecurityController security;
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    // Ensure unapproved policies remain untouched.
    TEST_ASSERT_FALSE(security.config().pir_triggers_alarm_in_away);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(SwitchArbitrationMode::DISABLED_PENDING_APPROVAL),
        static_cast<uint8_t>(security.config().switch_arbitration));

    // Step 1: Out-of-range servo command is admitted at envelope level, rejected by SecurityController.
    const CommandEnvelope bad_servo_env = makeValidNodeBEnvelope("cmd-nodeb-20261116-0003");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(guard.validate(bad_servo_env, clk).decision));

    NodeBCommand bad_servo_cmd;
    bad_servo_cmd.target = CommandTarget::SERVO;
    bad_servo_cmd.action = CommandAction::SET;
    bad_servo_cmd.has_int_value = true;
    bad_servo_cmd.int_value = 150;  // Exceeds default safe max 90
    CommandResult sec_res = security.applyCommand(bad_servo_cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED),
                            static_cast<uint8_t>(sec_res.status));
    if (sec_res.status == CommandStatus::APPLIED) {
        guard.recordProcessed(bad_servo_env.command_id);
    }
    TEST_ASSERT_FALSE(guard.isDuplicate(bad_servo_env.command_id));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));

    // Step 2: Valid AWAY command is admitted AND applied -> caller records it as processed.
    const CommandEnvelope away_env = makeValidNodeBEnvelope("cmd-nodeb-20261116-0001");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(guard.validate(away_env, clk).decision));
    NodeBCommand away_cmd;
    away_cmd.target = CommandTarget::SECURITY_MODE;
    away_cmd.action = CommandAction::SET;
    away_cmd.has_mode_value = true;
    away_cmd.mode_value = SecurityMode::AWAY;
    sec_res = security.applyCommand(away_cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(sec_res.status));
    if (sec_res.status == CommandStatus::APPLIED) {
        TEST_ASSERT_TRUE(guard.recordProcessed(away_env.command_id));
    }
    TEST_ASSERT_TRUE(guard.isDuplicate(away_env.command_id));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::DUPLICATE),
                            static_cast<uint8_t>(guard.validate(away_env, clk).ack_status));
}

void test_schema_version_and_device_id_rejected(void) {
    CommandGuard guard;
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    CommandEnvelope bad_ver = makeValidNodeBEnvelope("cmd-nodeb-0001");
    bad_ver.schema_version = 2;
    AdmissionResult r = guard.validate(bad_ver, clk);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                            static_cast<uint8_t>(r.decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::REJECTED),
                            static_cast<uint8_t>(r.ack_status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_SCHEMA_VERSION),
                            static_cast<uint8_t>(r.reason));

    const char* wrong_devices[] = {nullptr, "", "node-a", "node-b ", "node-B", "node-b-extra"};
    for (size_t i = 0; i < sizeof(wrong_devices) / sizeof(wrong_devices[0]); ++i) {
        CommandEnvelope bad_dev = makeValidNodeBEnvelope("cmd-nodeb-0002");
        bad_dev.device_id = wrong_devices[i];
        r = guard.validate(bad_dev, clk);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                                static_cast<uint8_t>(r.decision));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_DEVICE_ID),
                                static_cast<uint8_t>(r.reason));
    }
}

void test_command_id_length_and_charset_boundaries(void) {
    CommandGuard guard;
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    char id64[65];
    std::memset(id64, 'B', 64);
    id64[64] = '\0';

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardDecision::ADMIT),
        static_cast<uint8_t>(guard.validate(makeValidNodeBEnvelope("cmd_b001"), clk).decision));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardDecision::ADMIT),
        static_cast<uint8_t>(guard.validate(makeValidNodeBEnvelope(id64), clk).decision));

    char id65[66];
    std::memset(id65, 'C', 65);
    id65[65] = '\0';

    const char* bad_ids[] = {nullptr, "", "cmd-007", id65, "cmd b001", "cmd/b001", "cmd.b001", "cmd+b001"};
    for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); ++i) {
        AdmissionResult r = guard.validate(makeValidNodeBEnvelope(bad_ids[i]), clk);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                                static_cast<uint8_t>(r.decision));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_COMMAND_ID),
                                static_cast<uint8_t>(r.reason));
        TEST_ASSERT_FALSE(guard.recordProcessed(bad_ids[i]));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));
}

void test_ring_buffer_wraparound_and_reboot_reset(void) {
    CommandGuardConfig cfg;
    cfg.dedup_capacity = 4U;
    CommandGuard guard(cfg);
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-b-ring-0001"));
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-b-ring-0002"));
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-b-ring-0003"));
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-b-ring-0004"));
    TEST_ASSERT_EQUAL_UINT32(4U, static_cast<uint32_t>(guard.dedupCount()));

    // Re-recording an active ID must NOT evict oldest distinct ID.
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-b-ring-0002"));
    TEST_ASSERT_TRUE(guard.isDuplicate("cmd-b-ring-0001"));

    // 5th distinct ID wraps ring and evicts cmd-b-ring-0001.
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-b-ring-0005"));
    TEST_ASSERT_EQUAL_UINT32(4U, static_cast<uint32_t>(guard.dedupCount()));
    TEST_ASSERT_FALSE(guard.isDuplicate("cmd-b-ring-0001"));
    TEST_ASSERT_TRUE(guard.isDuplicate("cmd-b-ring-0002"));
    TEST_ASSERT_TRUE(guard.isDuplicate("cmd-b-ring-0005"));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardDecision::ADMIT),
        static_cast<uint8_t>(guard.validate(makeValidNodeBEnvelope("cmd-b-ring-0001"), clk).decision));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandAckStatus::DUPLICATE),
        static_cast<uint8_t>(guard.validate(makeValidNodeBEnvelope("cmd-b-ring-0005"), clk).ack_status));

    // Volatile RAM state resets cleanly (lost on reboot/power loss).
    guard.resetDedupHistory();
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));
    TEST_ASSERT_FALSE(guard.isDuplicate("cmd-b-ring-0005"));
}

void test_utc_expiry_boundaries(void) {
    CommandGuard guard;
    const CommandEnvelope cmd = makeValidNodeBEnvelope("cmd-b-exp-0001");

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardDecision::ADMIT),
        static_cast<uint8_t>(guard.validate(cmd, makeSyncedClock(kExpiresUtcMs - 1LL)).decision));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardDecision::ADMIT),
        static_cast<uint8_t>(guard.validate(cmd, makeSyncedClock(kExpiresUtcMs)).decision));

    AdmissionResult r_past = guard.validate(cmd, makeSyncedClock(kExpiresUtcMs + 1LL));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                            static_cast<uint8_t>(r_past.decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::EXPIRED),
                            static_cast<uint8_t>(r_past.ack_status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::COMMAND_EXPIRED_UTC),
                            static_cast<uint8_t>(r_past.reason));
}

void test_unsynced_clock_rejects_unverifiable_expiry(void) {
    CommandGuard guard;
    ClockContext unsynced;
    unsynced.time_synced = false;
    unsynced.now_utc_epoch_ms = 0LL;
    unsynced.now_uptime_ms = 12000ULL;
    unsynced.session_online = true;
    unsynced.session_online_uptime_ms = 5000ULL;
    unsynced.current_boot_id = "boot-b-0001-7c8d";

    CommandEnvelope cmd = makeValidNodeBEnvelope("cmd-b-unsync-01");
    AdmissionResult r = guard.validate(cmd, unsynced);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                            static_cast<uint8_t>(r.decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::EXPIRED),
                            static_cast<uint8_t>(r.ack_status));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardReason::CLOCK_UNSYNCED_CANNOT_VERIFY_EXPIRY),
        static_cast<uint8_t>(r.reason));

    // Even with require_time_sync=false, default policy stays strict until PROP-02 is approved.
    cmd.require_time_sync = false;
    r = guard.validate(cmd, unsynced);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardReason::CLOCK_UNSYNCED_CANNOT_VERIFY_EXPIRY),
        static_cast<uint8_t>(r.reason));
}

void test_invalid_time_values_and_overflow_safety(void) {
    CommandGuard guard;
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    CommandEnvelope no_exp = makeValidNodeBEnvelope("cmd-b-time-0001");
    no_exp.has_expires_at = false;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_EXPIRY),
                            static_cast<uint8_t>(guard.validate(no_exp, clk).reason));

    CommandEnvelope neg_exp = makeValidNodeBEnvelope("cmd-b-time-0002");
    neg_exp.expires_at_epoch_ms = 0LL;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_EXPIRY),
                            static_cast<uint8_t>(guard.validate(neg_exp, clk).reason));

    CommandEnvelope inverted = makeValidNodeBEnvelope("cmd-b-time-0003");
    inverted.issued_at_epoch_ms = kExpiresUtcMs + 1LL;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_EXPIRY),
                            static_cast<uint8_t>(guard.validate(inverted, clk).reason));

    CommandEnvelope bad_ttl = makeValidNodeBEnvelope("cmd-b-time-0004");
    bad_ttl.ttl_ms = -1LL;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_TTL),
                            static_cast<uint8_t>(guard.validate(bad_ttl, clk).reason));
    bad_ttl.ttl_ms = LLONG_MAX;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_TTL),
                            static_cast<uint8_t>(guard.validate(bad_ttl, clk).reason));

    ClockContext bad_clk = makeSyncedClock(-100LL);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardReason::INVALID_CLOCK),
        static_cast<uint8_t>(guard.validate(makeValidNodeBEnvelope("cmd-b-time-0005"), bad_clk).reason));

    CommandEnvelope future_cmd = makeValidNodeBEnvelope("cmd-b-time-0006");
    future_cmd.issued_at_epoch_ms = LLONG_MAX - 100LL;
    future_cmd.expires_at_epoch_ms = LLONG_MAX;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::ISSUED_IN_FUTURE),
                            static_cast<uint8_t>(guard.validate(future_cmd, clk).reason));
}

void test_opt_in_unsynced_session_ttl_and_invalid_config_fallback(void) {
    // Verify invalid constructor config safely falls back to node-b defaults (no dangling pointer).
    CommandGuardConfig bad_cfg;
    {
        char temp_buf[] = "node-b";
        bad_cfg.expected_device_id = temp_buf;
        bad_cfg.dedup_capacity = 0U;
        bad_cfg.max_ttl_ms = 0LL;
        bad_cfg.max_future_skew_ms = -5LL;
        CommandGuard copied_guard(bad_cfg);
        std::memset(temp_buf, 'X', sizeof(temp_buf) - 1U);
        TEST_ASSERT_EQUAL_STRING("node-b", copied_guard.expectedDeviceId());
        TEST_ASSERT_EQUAL_UINT32(24U, static_cast<uint32_t>(copied_guard.dedupCapacity()));
        TEST_ASSERT_EQUAL_INT64(86400000LL, copied_guard.config().max_ttl_ms);
        TEST_ASSERT_EQUAL_INT64(60000LL, copied_guard.config().max_future_skew_ms);
    }

    CommandGuardConfig opt_cfg;
    opt_cfg.allow_unsynced_session_ttl = true;
    CommandGuard guard(opt_cfg);

    ClockContext unsynced;
    unsynced.time_synced = false;
    unsynced.session_online = true;
    unsynced.session_online_uptime_ms = UINT64_MAX - 20000ULL;
    unsynced.now_uptime_ms = UINT64_MAX - 5000ULL;
    unsynced.current_boot_id = "boot-b-0009-c3e1";

    CommandEnvelope cmd = makeValidNodeBEnvelope("cmd-b-sess-0001");
    cmd.require_time_sync = false;
    cmd.target_boot_id = "boot-b-0009-c3e1";
    cmd.has_received_uptime_ms = true;
    cmd.received_uptime_ms = UINT64_MAX - 15000ULL;
    cmd.ttl_ms = 10000LL;  // elapsed = 10000 <= 10000 -> ADMIT

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(guard.validate(cmd, unsynced).decision));

    cmd.target_boot_id = "boot-b-0008-old0";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::BOOT_ID_MISMATCH),
                            static_cast<uint8_t>(guard.validate(cmd, unsynced).reason));

    cmd.target_boot_id = "boot-b-0009-c3e1";
    unsynced.now_uptime_ms = UINT64_MAX - 4999ULL;  // elapsed = 10001 > 10000 -> EXPIRED
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::COMMAND_EXPIRED_SESSION_TTL),
                            static_cast<uint8_t>(guard.validate(cmd, unsynced).reason));
}

void test_copy_and_move_construction_rebind_expected_device_id_across_source_scope(void) {
    CommandGuard copy_target;
    CommandGuard move_target;
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    {
        CommandGuard source =
            makeGuardFromScopedStackBuffer("node-b-sim01", 6U, 45000LL, 15000LL, true);
        TEST_ASSERT_EQUAL_STRING("node-b-sim01", source.expectedDeviceId());
        TEST_ASSERT_EQUAL_PTR(source.expectedDeviceId(), source.config().expected_device_id);
        TEST_ASSERT_TRUE(source.recordProcessed("cmd-b-src-0001"));

        CommandGuard copy_constructed(source);
        TEST_ASSERT_EQUAL_STRING("node-b-sim01", copy_constructed.expectedDeviceId());
        TEST_ASSERT_EQUAL_STRING("node-b-sim01", copy_constructed.config().expected_device_id);
        TEST_ASSERT_EQUAL_PTR(copy_constructed.expectedDeviceId(),
                              copy_constructed.config().expected_device_id);
        TEST_ASSERT_TRUE(copy_constructed.expectedDeviceId() != source.expectedDeviceId());
        TEST_ASSERT_EQUAL_UINT32(6U, static_cast<uint32_t>(copy_constructed.dedupCapacity()));
        TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(copy_constructed.dedupCount()));
        TEST_ASSERT_TRUE(copy_constructed.isDuplicate("cmd-b-src-0001"));

        // Mutate copy_constructed dedup independently of source.
        TEST_ASSERT_TRUE(copy_constructed.recordProcessed("cmd-b-copy-0002"));
        TEST_ASSERT_FALSE(source.isDuplicate("cmd-b-copy-0002"));

        CommandGuard move_constructed(std::move(source));
        TEST_ASSERT_EQUAL_STRING("node-b-sim01", move_constructed.expectedDeviceId());
        TEST_ASSERT_EQUAL_STRING("node-b-sim01", move_constructed.config().expected_device_id);
        TEST_ASSERT_EQUAL_PTR(move_constructed.expectedDeviceId(),
                              move_constructed.config().expected_device_id);
        TEST_ASSERT_TRUE(move_constructed.expectedDeviceId() != source.expectedDeviceId());
        TEST_ASSERT_EQUAL_PTR(source.expectedDeviceId(), source.config().expected_device_id);
        TEST_ASSERT_EQUAL_UINT32(6U, static_cast<uint32_t>(move_constructed.dedupCapacity()));
        TEST_ASSERT_EQUAL_INT64(45000LL, move_constructed.config().max_ttl_ms);
        TEST_ASSERT_EQUAL_INT64(15000LL, move_constructed.config().max_future_skew_ms);
        TEST_ASSERT_TRUE(move_constructed.config().allow_unsynced_session_ttl);
        TEST_ASSERT_TRUE(move_constructed.isDuplicate("cmd-b-src-0001"));
        TEST_ASSERT_FALSE(move_constructed.isDuplicate("cmd-b-copy-0002"));

        copy_target = copy_constructed;
        move_target = std::move(move_constructed);
    }

    // Inner source, copy_constructed, and move_constructed are now out of scope.
    TEST_ASSERT_EQUAL_PTR(copy_target.expectedDeviceId(), copy_target.config().expected_device_id);
    TEST_ASSERT_EQUAL_PTR(move_target.expectedDeviceId(), move_target.config().expected_device_id);
    TEST_ASSERT_EQUAL_STRING("node-b-sim01", copy_target.config().expected_device_id);
    TEST_ASSERT_EQUAL_STRING("node-b-sim01", move_target.config().expected_device_id);
    TEST_ASSERT_TRUE(copy_target.isDuplicate("cmd-b-src-0001"));
    TEST_ASSERT_TRUE(copy_target.isDuplicate("cmd-b-copy-0002"));
    TEST_ASSERT_TRUE(move_target.isDuplicate("cmd-b-src-0001"));
    TEST_ASSERT_FALSE(move_target.isDuplicate("cmd-b-copy-0002"));

    CommandEnvelope custom_ok = makeValidNodeBEnvelope("cmd-b-scope-0003");
    custom_ok.device_id = "node-b-sim01";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(copy_target.validate(custom_ok, clk).decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(move_target.validate(custom_ok, clk).decision));

    CommandEnvelope default_wrong = makeValidNodeBEnvelope("cmd-b-scope-0004");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_DEVICE_ID),
                            static_cast<uint8_t>(copy_target.validate(default_wrong, clk).reason));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_DEVICE_ID),
                            static_cast<uint8_t>(move_target.validate(default_wrong, clk).reason));
}

void test_copy_and_move_assignment_overwrite_state_and_rebind_buffer(void) {
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    CommandGuard dest_copy;
    TEST_ASSERT_EQUAL_STRING("node-b", dest_copy.expectedDeviceId());
    TEST_ASSERT_TRUE(dest_copy.recordProcessed("cmd-b-old-0001"));

    CommandGuard dest_move;
    TEST_ASSERT_TRUE(dest_move.recordProcessed("cmd-b-old-0002"));

    {
        char temp_id[] = "node-b-zone_02";
        CommandGuardConfig src_cfg;
        src_cfg.expected_device_id = temp_id;
        src_cfg.dedup_capacity = 3U;
        src_cfg.max_ttl_ms = 120000LL;
        src_cfg.max_future_skew_ms = 5000LL;
        src_cfg.allow_unsynced_session_ttl = true;

        CommandGuard src_copy(src_cfg);
        // Mutate temp_id immediately to verify constructor copied into src_copy buffer.
        std::memcpy(temp_id, "node-a-mutated", sizeof("node-a-mutated"));
        TEST_ASSERT_TRUE(src_copy.recordProcessed("cmd-b-new-0001"));
        TEST_ASSERT_TRUE(src_copy.recordProcessed("cmd-b-new-0002"));
        TEST_ASSERT_TRUE(src_copy.recordProcessed("cmd-b-new-0003"));

        dest_copy = src_copy;
        TEST_ASSERT_EQUAL_STRING("node-b-zone_02", dest_copy.expectedDeviceId());
        TEST_ASSERT_EQUAL_STRING("node-b-zone_02", dest_copy.config().expected_device_id);
        TEST_ASSERT_EQUAL_PTR(dest_copy.expectedDeviceId(), dest_copy.config().expected_device_id);
        TEST_ASSERT_TRUE(dest_copy.expectedDeviceId() != src_copy.expectedDeviceId());
        TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(dest_copy.dedupCapacity()));
        TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(dest_copy.dedupCount()));
        TEST_ASSERT_FALSE(dest_copy.isDuplicate("cmd-b-old-0001"));
        TEST_ASSERT_TRUE(dest_copy.isDuplicate("cmd-b-new-0001"));
        TEST_ASSERT_TRUE(dest_copy.recordProcessed("cmd-b-new-0004"));
        TEST_ASSERT_FALSE(dest_copy.isDuplicate("cmd-b-new-0001"));
        TEST_ASSERT_TRUE(dest_copy.isDuplicate("cmd-b-new-0004"));
        TEST_ASSERT_TRUE(src_copy.isDuplicate("cmd-b-new-0001"));
        TEST_ASSERT_FALSE(src_copy.isDuplicate("cmd-b-new-0004"));

        CommandGuard src_move(
            makeGuardFromScopedStackBuffer("node-b-move_03", 5U, 90000LL, 2000LL, false));
        TEST_ASSERT_TRUE(src_move.recordProcessed("cmd-b-mv-0001"));
        dest_move = std::move(src_move);

        TEST_ASSERT_EQUAL_STRING("node-b-move_03", dest_move.expectedDeviceId());
        TEST_ASSERT_EQUAL_STRING("node-b-move_03", dest_move.config().expected_device_id);
        TEST_ASSERT_EQUAL_PTR(dest_move.expectedDeviceId(), dest_move.config().expected_device_id);
        TEST_ASSERT_TRUE(dest_move.expectedDeviceId() != src_move.expectedDeviceId());
        TEST_ASSERT_EQUAL_PTR(src_move.expectedDeviceId(), src_move.config().expected_device_id);
        TEST_ASSERT_EQUAL_UINT32(5U, static_cast<uint32_t>(dest_move.dedupCapacity()));
        TEST_ASSERT_FALSE(dest_move.isDuplicate("cmd-b-old-0002"));
        TEST_ASSERT_TRUE(dest_move.isDuplicate("cmd-b-mv-0001"));
    }

    // Source objects are out of scope; destination guards must validate cleanly.
    CommandEnvelope copy_env = makeValidNodeBEnvelope("cmd-b-assign-001");
    copy_env.device_id = "node-b-zone_02";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(dest_copy.validate(copy_env, clk).decision));

    CommandEnvelope move_env = makeValidNodeBEnvelope("cmd-b-assign-002");
    move_env.device_id = "node-b-move_03";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(dest_move.validate(move_env, clk).decision));
}

void test_self_assignment_and_temporary_max_length_buffer_and_invalid_config_fallback(void) {
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    // 1. Self copy-assignment and self move-assignment must be safe and keep all state intact.
    CommandGuard self_guard =
        makeGuardFromScopedStackBuffer("node-b-self_01", 8U, 60000LL, 10000LL, false);
    TEST_ASSERT_TRUE(self_guard.recordProcessed("cmd-b-self-0001"));

    CommandGuard* self_alias = &self_guard;
    self_guard = *self_alias;
    TEST_ASSERT_EQUAL_STRING("node-b-self_01", self_guard.expectedDeviceId());
    TEST_ASSERT_EQUAL_PTR(self_guard.expectedDeviceId(), self_guard.config().expected_device_id);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(self_guard.dedupCount()));
    TEST_ASSERT_TRUE(self_guard.isDuplicate("cmd-b-self-0001"));

    self_guard = std::move(*self_alias);
    TEST_ASSERT_EQUAL_STRING("node-b-self_01", self_guard.expectedDeviceId());
    TEST_ASSERT_EQUAL_PTR(self_guard.expectedDeviceId(), self_guard.config().expected_device_id);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(self_guard.dedupCount()));
    TEST_ASSERT_TRUE(self_guard.isDuplicate("cmd-b-self-0001"));

    CommandEnvelope self_env = makeValidNodeBEnvelope("cmd-b-self-0002");
    self_env.device_id = "node-b-self_01";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(self_guard.validate(self_env, clk).decision));

    // 2. Temporary buffer at exact kMaxDeviceIdLen (32 chars) copied and moved safely.
    char max_id_buf[CommandGuard::kMaxDeviceIdLen + 1U];
    std::memset(max_id_buf, 'B', CommandGuard::kMaxDeviceIdLen);
    max_id_buf[CommandGuard::kMaxDeviceIdLen] = '\0';

    char expected_max[CommandGuard::kMaxDeviceIdLen + 1U];
    std::memcpy(expected_max, max_id_buf, sizeof(expected_max));

    CommandGuard max_copy;
    {
        CommandGuardConfig max_cfg;
        max_cfg.expected_device_id = max_id_buf;
        CommandGuard max_src(max_cfg);
        std::memset(max_id_buf, 'X', CommandGuard::kMaxDeviceIdLen);
        max_copy = max_src;
        TEST_ASSERT_EQUAL_PTR(max_copy.expectedDeviceId(), max_copy.config().expected_device_id);
        TEST_ASSERT_TRUE(max_copy.expectedDeviceId() != max_src.expectedDeviceId());
    }
    TEST_ASSERT_EQUAL_STRING(expected_max, max_copy.expectedDeviceId());
    TEST_ASSERT_EQUAL_PTR(max_copy.expectedDeviceId(), max_copy.config().expected_device_id);
    CommandEnvelope max_env = makeValidNodeBEnvelope("cmd-b-max32-0001");
    max_env.device_id = expected_max;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(max_copy.validate(max_env, clk).decision));

    // 3. Invalid expected_device_id configs fall back to "node-b" and preserve pointer invariant across copy/move.
    char too_long[CommandGuard::kMaxDeviceIdLen + 2U];
    std::memset(too_long, 'b', CommandGuard::kMaxDeviceIdLen + 1U);
    too_long[CommandGuard::kMaxDeviceIdLen + 1U] = '\0';

    const char* bad_device_ids[] = {
        nullptr,
        "",
        too_long,
        "node b",
        "node.b",
        "node/b",
        "node-b!",
        "node$b",
    };

    for (size_t i = 0; i < sizeof(bad_device_ids) / sizeof(bad_device_ids[0]); ++i) {
        CommandGuard fallback_copy;
        CommandGuard fallback_move;
        {
            CommandGuardConfig bad_cfg;
            bad_cfg.expected_device_id = bad_device_ids[i];
            bad_cfg.dedup_capacity = 999U;
            bad_cfg.max_ttl_ms = -10LL;
            bad_cfg.max_future_skew_ms = -1LL;

            CommandGuard fallback_src(bad_cfg);
            TEST_ASSERT_EQUAL_STRING("node-b", fallback_src.expectedDeviceId());
            TEST_ASSERT_EQUAL_PTR(fallback_src.expectedDeviceId(),
                                  fallback_src.config().expected_device_id);

            fallback_copy = fallback_src;
            CommandGuard moved_tmp(std::move(fallback_src));
            fallback_move = std::move(moved_tmp);
        }

        TEST_ASSERT_EQUAL_STRING("node-b", fallback_copy.expectedDeviceId());
        TEST_ASSERT_EQUAL_PTR(fallback_copy.expectedDeviceId(),
                              fallback_copy.config().expected_device_id);
        TEST_ASSERT_EQUAL_UINT32(24U, static_cast<uint32_t>(fallback_copy.dedupCapacity()));
        TEST_ASSERT_EQUAL_INT64(86400000LL, fallback_copy.config().max_ttl_ms);
        TEST_ASSERT_EQUAL_INT64(60000LL, fallback_copy.config().max_future_skew_ms);

        TEST_ASSERT_EQUAL_STRING("node-b", fallback_move.expectedDeviceId());
        TEST_ASSERT_EQUAL_PTR(fallback_move.expectedDeviceId(),
                              fallback_move.config().expected_device_id);

        CommandEnvelope ok_default = makeValidNodeBEnvelope("cmd-b-fallback-01");
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                                static_cast<uint8_t>(fallback_copy.validate(ok_default, clk).decision));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                                static_cast<uint8_t>(fallback_move.validate(ok_default, clk).decision));

        CommandEnvelope reject_other = makeValidNodeBEnvelope("cmd-b-fallback-02");
        reject_other.device_id = (bad_device_ids[i] != nullptr) ? bad_device_ids[i] : "node-a";
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(CommandGuardReason::INVALID_DEVICE_ID),
            static_cast<uint8_t>(fallback_copy.validate(reject_other, clk).reason));
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(CommandGuardReason::INVALID_DEVICE_ID),
            static_cast<uint8_t>(fallback_move.validate(reject_other, clk).reason));
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_valid_node_b_command_admitted_and_validate_does_not_mark_dedup);
    RUN_TEST(test_two_phase_security_apply_keeps_rejected_command_unmarked_and_preserves_policies);
    RUN_TEST(test_schema_version_and_device_id_rejected);
    RUN_TEST(test_command_id_length_and_charset_boundaries);
    RUN_TEST(test_ring_buffer_wraparound_and_reboot_reset);
    RUN_TEST(test_utc_expiry_boundaries);
    RUN_TEST(test_unsynced_clock_rejects_unverifiable_expiry);
    RUN_TEST(test_invalid_time_values_and_overflow_safety);
    RUN_TEST(test_opt_in_unsynced_session_ttl_and_invalid_config_fallback);
    RUN_TEST(test_copy_and_move_construction_rebind_expected_device_id_across_source_scope);
    RUN_TEST(test_copy_and_move_assignment_overwrite_state_and_rebind_buffer);
    RUN_TEST(test_self_assignment_and_temporary_max_length_buffer_and_invalid_config_fallback);
    return UNITY_END();
}
