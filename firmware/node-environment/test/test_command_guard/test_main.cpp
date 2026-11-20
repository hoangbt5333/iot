#include <unity.h>

#include <climits>
#include <cstring>

#include "command_guard.h"

using node_env::AdmissionResult;
using node_env::ClockContext;
using node_env::CommandAckStatus;
using node_env::CommandEnvelope;
using node_env::CommandGuard;
using node_env::CommandGuardConfig;
using node_env::CommandGuardDecision;
using node_env::CommandGuardReason;

namespace {

constexpr int64_t kIssuedUtcMs = 1794793210000LL;   // 2026-11-16T01:40:10Z
constexpr int64_t kExpiresUtcMs = 1794793240000LL;  // 2026-11-16T01:40:40Z

CommandEnvelope makeValidEnvelope(const char* command_id) {
    CommandEnvelope env;
    env.schema_version = 1;
    env.device_id = "node-a";
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
    clk.current_boot_id = "boot-a-0001-9f2c";
    return clk;
}

CommandGuard makeGuardFromScopedStackBuffer(const char* src_id) {
    char scoped_buf[CommandGuard::kMaxDeviceIdLen + 2U]{};
    const size_t len = std::strlen(src_id);
    if (len < sizeof(scoped_buf)) {
        std::memcpy(scoped_buf, src_id, len + 1U);
    }
    CommandGuardConfig cfg;
    cfg.expected_device_id = scoped_buf;
    CommandGuard guard(cfg);
    // Mutate local buffer before scope ends; returned guard must remain self-contained.
    std::memset(scoped_buf, 'Z', sizeof(scoped_buf) - 1U);
    scoped_buf[sizeof(scoped_buf) - 1U] = '\0';
    return guard;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_valid_command_admitted_and_validate_does_not_mark_dedup(void) {
    CommandGuard guard;
    TEST_ASSERT_EQUAL_UINT32(24U, static_cast<uint32_t>(guard.dedupCapacity()));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));

    const CommandEnvelope cmd = makeValidEnvelope("cmd-nodea-20261116-0001");
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    // Calling validate() repeatedly before automation applies must NOT mark duplicate.
    AdmissionResult r1 = guard.validate(cmd, clk);
    AdmissionResult r2 = guard.validate(cmd, clk);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(r1.decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(r2.decision));
    TEST_ASSERT_FALSE(guard.isDuplicate(cmd.command_id));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));

    // Only after explicit recordProcessed() does the ID become duplicate.
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

void test_schema_version_and_device_id_rejected(void) {
    CommandGuard guard;
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    CommandEnvelope bad_ver = makeValidEnvelope("cmd-nodea-0001");
    bad_ver.schema_version = 2;
    AdmissionResult r = guard.validate(bad_ver, clk);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                            static_cast<uint8_t>(r.decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::REJECTED),
                            static_cast<uint8_t>(r.ack_status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_SCHEMA_VERSION),
                            static_cast<uint8_t>(r.reason));

    const char* bad_devices[] = {nullptr, "", "node-b", "node-a ", "node-A"};
    for (size_t i = 0; i < sizeof(bad_devices) / sizeof(bad_devices[0]); ++i) {
        CommandEnvelope bad_dev = makeValidEnvelope("cmd-nodea-0002");
        bad_dev.device_id = bad_devices[i];
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

    // Exactly 8 chars and 64 chars with [A-Za-z0-9_-] must be admitted.
    char id64[65];
    std::memset(id64, 'A', 64);
    id64[64] = '\0';

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardDecision::ADMIT),
        static_cast<uint8_t>(guard.validate(makeValidEnvelope("cmd_0001"), clk).decision));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardDecision::ADMIT),
        static_cast<uint8_t>(guard.validate(makeValidEnvelope(id64), clk).decision));

    // 7 chars, 65 chars, nullptr, empty, and invalid punctuation must be rejected.
    char id65[66];
    std::memset(id65, 'B', 65);
    id65[65] = '\0';

    const char* bad_ids[] = {nullptr, "", "cmd-007", id65, "cmd 0001", "cmd/0001", "cmd.0001", "cmd#0001"};
    for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); ++i) {
        AdmissionResult r = guard.validate(makeValidEnvelope(bad_ids[i]), clk);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                                static_cast<uint8_t>(r.decision));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_COMMAND_ID),
                                static_cast<uint8_t>(r.reason));
        TEST_ASSERT_FALSE(guard.recordProcessed(bad_ids[i]));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));
}

void test_ring_buffer_wraparound_and_duplicate_no_evict(void) {
    CommandGuardConfig cfg;
    cfg.dedup_capacity = 4U;
    CommandGuard guard(cfg);
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-ring-0001"));
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-ring-0002"));
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-ring-0003"));
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-ring-0004"));
    TEST_ASSERT_EQUAL_UINT32(4U, static_cast<uint32_t>(guard.dedupCount()));

    // Re-recording an existing ID must NOT advance write pointer or evict cmd-ring-0001.
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-ring-0002"));
    TEST_ASSERT_TRUE(guard.isDuplicate("cmd-ring-0001"));

    // Inserting 5th distinct ID wraps around and evicts oldest (cmd-ring-0001).
    TEST_ASSERT_TRUE(guard.recordProcessed("cmd-ring-0005"));
    TEST_ASSERT_EQUAL_UINT32(4U, static_cast<uint32_t>(guard.dedupCount()));
    TEST_ASSERT_FALSE(guard.isDuplicate("cmd-ring-0001"));
    TEST_ASSERT_TRUE(guard.isDuplicate("cmd-ring-0002"));
    TEST_ASSERT_TRUE(guard.isDuplicate("cmd-ring-0003"));
    TEST_ASSERT_TRUE(guard.isDuplicate("cmd-ring-0004"));
    TEST_ASSERT_TRUE(guard.isDuplicate("cmd-ring-0005"));

    // Evicted ID is admitted again when re-validated, while active ones are rejected as duplicate.
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardDecision::ADMIT),
        static_cast<uint8_t>(guard.validate(makeValidEnvelope("cmd-ring-0001"), clk).decision));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandAckStatus::DUPLICATE),
        static_cast<uint8_t>(guard.validate(makeValidEnvelope("cmd-ring-0005"), clk).ack_status));

    // Reset clears volatile RAM state (simulating reboot loss).
    guard.resetDedupHistory();
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));
    TEST_ASSERT_FALSE(guard.isDuplicate("cmd-ring-0005"));
}

void test_utc_expiry_boundaries(void) {
    CommandGuard guard;
    const CommandEnvelope cmd = makeValidEnvelope("cmd-exp-0001");

    // 1 ms before expiry -> ADMIT.
    AdmissionResult r_before = guard.validate(cmd, makeSyncedClock(kExpiresUtcMs - 1LL));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(r_before.decision));

    // Exactly at expires_at -> ADMIT (contract checks now_utc > expires_at).
    AdmissionResult r_exact = guard.validate(cmd, makeSyncedClock(kExpiresUtcMs));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(r_exact.decision));

    // 1 ms past expires_at -> EXPIRED.
    AdmissionResult r_past = guard.validate(cmd, makeSyncedClock(kExpiresUtcMs + 1LL));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                            static_cast<uint8_t>(r_past.decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::EXPIRED),
                            static_cast<uint8_t>(r_past.ack_status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::COMMAND_EXPIRED_UTC),
                            static_cast<uint8_t>(r_past.reason));
}

void test_unsynced_clock_rejects_absolute_expiry_commands(void) {
    CommandGuard guard;
    ClockContext unsynced;
    unsynced.time_synced = false;
    unsynced.now_utc_epoch_ms = 0LL;
    unsynced.now_uptime_ms = 12000ULL;
    unsynced.session_online = true;
    unsynced.session_online_uptime_ms = 5000ULL;
    unsynced.current_boot_id = "boot-a-0001-9f2c";

    CommandEnvelope cmd = makeValidEnvelope("cmd-unsync-0001");
    AdmissionResult r = guard.validate(cmd, unsynced);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                            static_cast<uint8_t>(r.decision));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::EXPIRED),
                            static_cast<uint8_t>(r.ack_status));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardReason::CLOCK_UNSYNCED_CANNOT_VERIFY_EXPIRY),
        static_cast<uint8_t>(r.reason));

    // Even when caller sets require_time_sync=false, default policy rejects because PROP-02 is unapproved.
    cmd.require_time_sync = false;
    r = guard.validate(cmd, unsynced);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::REJECT),
                            static_cast<uint8_t>(r.decision));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CommandGuardReason::CLOCK_UNSYNCED_CANNOT_VERIFY_EXPIRY),
        static_cast<uint8_t>(r.reason));
}

void test_invalid_time_values_and_overflow_safety(void) {
    CommandGuard guard;
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    // Missing or non-positive expires_at rejected.
    CommandEnvelope no_exp = makeValidEnvelope("cmd-time-0001");
    no_exp.has_expires_at = false;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_EXPIRY),
                            static_cast<uint8_t>(guard.validate(no_exp, clk).reason));

    CommandEnvelope neg_exp = makeValidEnvelope("cmd-time-0002");
    neg_exp.expires_at_epoch_ms = -500LL;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_EXPIRY),
                            static_cast<uint8_t>(guard.validate(neg_exp, clk).reason));

    // Inverted window (issued_at >= expires_at) rejected.
    CommandEnvelope inverted = makeValidEnvelope("cmd-time-0003");
    inverted.issued_at_epoch_ms = kExpiresUtcMs;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_EXPIRY),
                            static_cast<uint8_t>(guard.validate(inverted, clk).reason));

    // Negative / zero / huge TTL rejected.
    CommandEnvelope bad_ttl = makeValidEnvelope("cmd-time-0004");
    bad_ttl.ttl_ms = 0LL;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_TTL),
                            static_cast<uint8_t>(guard.validate(bad_ttl, clk).reason));
    bad_ttl.ttl_ms = LLONG_MAX;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_TTL),
                            static_cast<uint8_t>(guard.validate(bad_ttl, clk).reason));

    // Synced clock claiming epoch <= 0 rejected.
    ClockContext bad_clk = makeSyncedClock(0LL);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_CLOCK),
                            static_cast<uint8_t>(guard.validate(makeValidEnvelope("cmd-time-0005"), bad_clk).reason));

    // Issued too far in the future (near LLONG_MAX overflow guard) rejected without UB.
    CommandEnvelope future_cmd = makeValidEnvelope("cmd-time-0006");
    future_cmd.issued_at_epoch_ms = LLONG_MAX - 100LL;
    future_cmd.expires_at_epoch_ms = LLONG_MAX;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::ISSUED_IN_FUTURE),
                            static_cast<uint8_t>(guard.validate(future_cmd, clk).reason));
}

void test_opt_in_unsynced_session_ttl_checks_boot_id_and_uptime(void) {
    CommandGuardConfig cfg;
    cfg.allow_unsynced_session_ttl = true;
    CommandGuard guard(cfg);

    ClockContext unsynced;
    unsynced.time_synced = false;
    unsynced.session_online = true;
    unsynced.session_online_uptime_ms = 10000ULL;
    unsynced.now_uptime_ms = 25000ULL;
    unsynced.current_boot_id = "boot-a-0009-ab12";

    CommandEnvelope cmd = makeValidEnvelope("cmd-sess-0001");
    cmd.require_time_sync = false;
    cmd.target_boot_id = "boot-a-0009-ab12";
    cmd.has_received_uptime_ms = true;
    cmd.received_uptime_ms = 15000ULL;
    cmd.ttl_ms = 10000LL;  // elapsed = 25000 - 15000 = 10000 <= 10000 -> ADMIT

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(guard.validate(cmd, unsynced).decision));

    // Mismatched boot_id rejected.
    cmd.target_boot_id = "boot-a-0008-old0";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::BOOT_ID_MISMATCH),
                            static_cast<uint8_t>(guard.validate(cmd, unsynced).reason));

    // 1 ms past relative TTL -> EXPIRED.
    cmd.target_boot_id = "boot-a-0009-ab12";
    unsynced.now_uptime_ms = 25001ULL;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::COMMAND_EXPIRED_SESSION_TTL),
                            static_cast<uint8_t>(guard.validate(cmd, unsynced).reason));

    // Clock inversion (received_uptime > now_uptime) rejected safely without underflow.
    unsynced.now_uptime_ms = 14000ULL;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_CLOCK),
                            static_cast<uint8_t>(guard.validate(cmd, unsynced).reason));

    // High-bit uint64 uptime values near UINT64_MAX work without overflow.
    unsynced.session_online_uptime_ms = UINT64_MAX - 20000ULL;
    cmd.received_uptime_ms = UINT64_MAX - 10000ULL;
    unsynced.now_uptime_ms = UINT64_MAX - 5000ULL;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(guard.validate(cmd, unsynced).decision));
}

void test_constructor_invalid_config_falls_back_safely_and_unapplied_command_not_marked(void) {
    CommandGuardConfig bad_cfg;
    bad_cfg.expected_device_id = nullptr;
    bad_cfg.dedup_capacity = 999U;  // Exceeds kMaxDedupCapacity
    bad_cfg.max_ttl_ms = -10LL;
    bad_cfg.max_future_skew_ms = -1LL;

    CommandGuard guard(bad_cfg);
    TEST_ASSERT_EQUAL_STRING("node-a", guard.config().expected_device_id);
    TEST_ASSERT_EQUAL_UINT32(24U, static_cast<uint32_t>(guard.dedupCapacity()));
    TEST_ASSERT_EQUAL_INT64(86400000LL, guard.config().max_ttl_ms);
    TEST_ASSERT_EQUAL_INT64(60000LL, guard.config().max_future_skew_ms);

    const CommandEnvelope cmd = makeValidEnvelope("cmd-fail-0001");
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(guard.validate(cmd, clk).decision));

    // Simulate downstream automation rejection -> caller skips recordProcessed().
    const bool automation_applied = false;
    if (automation_applied) {
        guard.recordProcessed(cmd.command_id);
    }
    TEST_ASSERT_FALSE(guard.isDuplicate(cmd.command_id));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(guard.dedupCount()));
}

void test_expected_device_id_copied_and_independent_of_caller_buffer_mutation(void) {
    char caller_buf[24] = "node-a-sim01";
    CommandGuardConfig cfg;
    cfg.expected_device_id = caller_buf;

    CommandGuard guard(cfg);
    TEST_ASSERT_EQUAL_STRING("node-a-sim01", guard.expectedDeviceId());
    TEST_ASSERT_EQUAL_STRING("node-a-sim01", guard.config().expected_device_id);
    TEST_ASSERT_EQUAL_PTR(guard.expectedDeviceId(), guard.config().expected_device_id);
    TEST_ASSERT_TRUE(guard.expectedDeviceId() != caller_buf);

    // Mutate caller buffer in place after construction (no UAF/UB; stays valid memory).
    std::memcpy(caller_buf, "node-b", sizeof("node-b"));
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    TEST_ASSERT_EQUAL_STRING("node-a-sim01", guard.expectedDeviceId());
    TEST_ASSERT_EQUAL_STRING("node-a-sim01", guard.config().expected_device_id);

    CommandEnvelope custom_cmd = makeValidEnvelope("cmd-mut-0001");
    custom_cmd.device_id = "node-a-sim01";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(guard.validate(custom_cmd, clk).decision));
    TEST_ASSERT_TRUE(guard.recordProcessed(custom_cmd.command_id));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandAckStatus::DUPLICATE),
                            static_cast<uint8_t>(guard.validate(custom_cmd, clk).ack_status));

    // Mutated buffer content and default "node-a" must both be rejected.
    CommandEnvelope wrong_mut_cmd = makeValidEnvelope("cmd-mut-0002");
    wrong_mut_cmd.device_id = caller_buf;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_DEVICE_ID),
                            static_cast<uint8_t>(guard.validate(wrong_mut_cmd, clk).reason));

    CommandEnvelope default_cmd = makeValidEnvelope("cmd-mut-0003");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_DEVICE_ID),
                            static_cast<uint8_t>(guard.validate(default_cmd, clk).reason));
}

void test_expected_device_id_survives_caller_scope_and_guard_copy(void) {
    CommandGuard assigned_guard;
    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);

    {
        CommandGuard scoped_guard = makeGuardFromScopedStackBuffer("node-a-env_02");
        TEST_ASSERT_EQUAL_STRING("node-a-env_02", scoped_guard.expectedDeviceId());
        TEST_ASSERT_EQUAL_PTR(scoped_guard.expectedDeviceId(), scoped_guard.config().expected_device_id);

        CommandGuard copy_constructed(scoped_guard);
        TEST_ASSERT_EQUAL_STRING("node-a-env_02", copy_constructed.expectedDeviceId());
        TEST_ASSERT_EQUAL_PTR(copy_constructed.expectedDeviceId(),
                              copy_constructed.config().expected_device_id);
        TEST_ASSERT_TRUE(copy_constructed.expectedDeviceId() != scoped_guard.expectedDeviceId());

        assigned_guard = copy_constructed;
        TEST_ASSERT_EQUAL_PTR(assigned_guard.expectedDeviceId(),
                              assigned_guard.config().expected_device_id);
        TEST_ASSERT_TRUE(assigned_guard.expectedDeviceId() != copy_constructed.expectedDeviceId());
    }

    // Both temporary factory buffer and inner CommandGuard copies are out of scope now.
    CommandEnvelope ok_cmd = makeValidEnvelope("cmd-scope-0001");
    ok_cmd.device_id = "node-a-env_02";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(assigned_guard.validate(ok_cmd, clk).decision));

    CommandEnvelope wrong_cmd = makeValidEnvelope("cmd-scope-0002");
    wrong_cmd.device_id = "node-a";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_DEVICE_ID),
                            static_cast<uint8_t>(assigned_guard.validate(wrong_cmd, clk).reason));

    // Exact max boundary length kMaxDeviceIdLen (32 chars) must be accepted and copied.
    char max_id_buf[CommandGuard::kMaxDeviceIdLen + 1U];
    std::memset(max_id_buf, 'N', CommandGuard::kMaxDeviceIdLen);
    max_id_buf[CommandGuard::kMaxDeviceIdLen] = '\0';

    char expected_max[CommandGuard::kMaxDeviceIdLen + 1U];
    std::memcpy(expected_max, max_id_buf, sizeof(expected_max));

    CommandGuardConfig max_cfg;
    max_cfg.expected_device_id = max_id_buf;
    CommandGuard max_guard(max_cfg);
    std::memset(max_id_buf, 'X', CommandGuard::kMaxDeviceIdLen);

    TEST_ASSERT_EQUAL_STRING(expected_max, max_guard.expectedDeviceId());
    CommandEnvelope max_cmd = makeValidEnvelope("cmd-max32-0001");
    max_cmd.device_id = expected_max;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                            static_cast<uint8_t>(max_guard.validate(max_cmd, clk).decision));
}

void test_invalid_expected_device_id_configs_fallback_safely(void) {
    char too_long[CommandGuard::kMaxDeviceIdLen + 2U];
    std::memset(too_long, 'a', CommandGuard::kMaxDeviceIdLen + 1U);
    too_long[CommandGuard::kMaxDeviceIdLen + 1U] = '\0';

    const char* bad_device_ids[] = {
        nullptr,
        "",
        too_long,
        "node a",
        "node.a",
        "node/a",
        "node-a!",
        "node$a",
    };

    const ClockContext clk = makeSyncedClock(kIssuedUtcMs + 1000LL);
    for (size_t i = 0; i < sizeof(bad_device_ids) / sizeof(bad_device_ids[0]); ++i) {
        CommandGuardConfig cfg;
        cfg.expected_device_id = bad_device_ids[i];
        CommandGuard guard(cfg);

        TEST_ASSERT_EQUAL_STRING("node-a", guard.expectedDeviceId());
        TEST_ASSERT_EQUAL_STRING("node-a", guard.config().expected_device_id);
        TEST_ASSERT_EQUAL_PTR(guard.expectedDeviceId(), guard.config().expected_device_id);

        CommandEnvelope valid_default = makeValidEnvelope("cmd-fallback-0001");
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardDecision::ADMIT),
                                static_cast<uint8_t>(guard.validate(valid_default, clk).decision));

        CommandEnvelope other = makeValidEnvelope("cmd-fallback-0002");
        other.device_id = (bad_device_ids[i] != nullptr) ? bad_device_ids[i] : "node-b";
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandGuardReason::INVALID_DEVICE_ID),
                                static_cast<uint8_t>(guard.validate(other, clk).reason));
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_valid_command_admitted_and_validate_does_not_mark_dedup);
    RUN_TEST(test_schema_version_and_device_id_rejected);
    RUN_TEST(test_command_id_length_and_charset_boundaries);
    RUN_TEST(test_ring_buffer_wraparound_and_duplicate_no_evict);
    RUN_TEST(test_utc_expiry_boundaries);
    RUN_TEST(test_unsynced_clock_rejects_absolute_expiry_commands);
    RUN_TEST(test_invalid_time_values_and_overflow_safety);
    RUN_TEST(test_opt_in_unsynced_session_ttl_checks_boot_id_and_uptime);
    RUN_TEST(test_constructor_invalid_config_falls_back_safely_and_unapplied_command_not_marked);
    RUN_TEST(test_expected_device_id_copied_and_independent_of_caller_buffer_mutation);
    RUN_TEST(test_expected_device_id_survives_caller_scope_and_guard_copy);
    RUN_TEST(test_invalid_expected_device_id_configs_fallback_safely);
    return UNITY_END();
}
