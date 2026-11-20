#include <unity.h>

#include <cstdint>

#include "node_security.h"
#include "security_inputs.h"

using node_sec::CommandStatus;
using node_sec::ISecurityInputSource;
using node_sec::MappedSecurityInput;
using node_sec::RawSecurityInputSample;
using node_sec::SecurityController;
using node_sec::SecurityInputPinConfig;
using node_sec::SecurityInputPolarityConfig;
using node_sec::SecurityInputSampler;
using node_sec::SecurityInputSamplerConfig;
using node_sec::SecurityMode;
using node_sec::SecuritySamplerStep;
using node_sec::SecurityState;
using node_sec::SwitchArbitrationMode;
using node_sec::TriggerSource;

namespace {

class FakeSecurityInputSource final : public ISecurityInputSource {
public:
    bool begin() override {
        ++begin_calls;
        if (!begin_succeeds) {
            initialized = false;
            return false;
        }
        initialized = true;
        return true;
    }

    RawSecurityInputSample readInputs() override {
        ++read_calls;
        if (!initialized) {
            return RawSecurityInputSample{};
        }
        return next_sample;
    }

    void setRawLevels(bool pir_high, bool reed_high, bool valid = true) {
        next_sample.valid = valid;
        next_sample.pir_level_high = pir_high;
        next_sample.reed_level_high = reed_high;
    }

    bool begin_succeeds = true;
    bool initialized = false;
    uint32_t begin_calls = 0;
    uint32_t read_calls = 0;
    RawSecurityInputSample next_sample{};
};

SecurityInputSamplerConfig makeImmediateSamplerConfig() {
    SecurityInputSamplerConfig cfg;
    cfg.sample_interval_ms = 0UL;
    return cfg;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_safe_init_and_uninitialized_or_invalid_inputs_do_not_emit_events(void) {
    SecurityController ctrl;
    FakeSecurityInputSource fake;
    // Simulate raw HIGH levels before begin() is called; source must report valid=false.
    fake.next_sample.valid = true;
    fake.next_sample.pir_level_high = true;
    fake.next_sample.reed_level_high = true;

    SecurityInputSampler sampler(makeImmediateSamplerConfig());

    for (uint32_t t = 0UL; t <= 200UL; t += 50UL) {
        const SecuritySamplerStep step = sampler.poll(fake, ctrl, t);
        TEST_ASSERT_TRUE(step.polled);
        TEST_ASSERT_FALSE(step.sample_valid);
        TEST_ASSERT_FALSE(step.door_outcome.door_changed);
        TEST_ASSERT_FALSE(step.motion_outcome.motion_rising_edge);
        TEST_ASSERT_FALSE(step.any_alarm_triggered);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::HOME),
                                static_cast<uint8_t>(step.state.mode));
        TEST_ASSERT_FALSE(step.state.door_open);
        TEST_ASSERT_FALSE(step.state.motion_detected);
        TEST_ASSERT_FALSE(step.state.alarm_on);
    }
    TEST_ASSERT_FALSE(sampler.hasValidSample());

    // Even after begin(), explicit valid=false reads must not fabricate door_open or alarm.
    TEST_ASSERT_TRUE(fake.begin());
    fake.setRawLevels(true, true, false);
    const SecuritySamplerStep invalid_step = sampler.poll(fake, ctrl, 300UL);
    TEST_ASSERT_TRUE(invalid_step.polled);
    TEST_ASSERT_FALSE(invalid_step.sample_valid);
    TEST_ASSERT_FALSE(ctrl.state().door_open);
    TEST_ASSERT_FALSE(ctrl.state().motion_detected);
    TEST_ASSERT_FALSE(ctrl.state().alarm_on);
}

void test_reed_and_pir_polarity_mapping_and_centralized_pin_defaults(void) {
    SecurityInputPinConfig pin_cfg;
    TEST_ASSERT_EQUAL_UINT8(27U, pin_cfg.pir_pin);
    TEST_ASSERT_EQUAL_UINT8(32U, pin_cfg.reed_pin);
    TEST_ASSERT_TRUE(pin_cfg.reed_use_internal_pullup);

    SecurityInputPolarityConfig default_pol;
    TEST_ASSERT_TRUE(default_pol.pir_motion_active_high);
    TEST_ASSERT_TRUE(default_pol.reed_door_open_high);

    // Proposed INPUT_PULLUP reed wiring: LOW => CLOSED (door_raw_open=false), HIGH => OPEN (true).
    MappedSecurityInput closed_and_idle =
        SecurityInputSampler::mapRawSample({true, false, false}, default_pol);
    TEST_ASSERT_TRUE(closed_and_idle.valid);
    TEST_ASSERT_FALSE(closed_and_idle.pir_motion_detected);
    TEST_ASSERT_FALSE(closed_and_idle.door_raw_open);

    MappedSecurityInput open_and_motion =
        SecurityInputSampler::mapRawSample({true, true, true}, default_pol);
    TEST_ASSERT_TRUE(open_and_motion.valid);
    TEST_ASSERT_TRUE(open_and_motion.pir_motion_detected);
    TEST_ASSERT_TRUE(open_and_motion.door_raw_open);

    // Verify centralized polarity handles inverted physical reed/PIR modules cleanly.
    SecurityInputPolarityConfig inverted_pol;
    inverted_pol.pir_motion_active_high = false;
    inverted_pol.reed_door_open_high = false;

    MappedSecurityInput inv_active =
        SecurityInputSampler::mapRawSample({true, false, false}, inverted_pol);
    TEST_ASSERT_TRUE(inv_active.valid);
    TEST_ASSERT_TRUE(inv_active.pir_motion_detected);
    TEST_ASSERT_TRUE(inv_active.door_raw_open);

    MappedSecurityInput inv_idle =
        SecurityInputSampler::mapRawSample({true, true, true}, inverted_pol);
    TEST_ASSERT_TRUE(inv_idle.valid);
    TEST_ASSERT_FALSE(inv_idle.pir_motion_detected);
    TEST_ASSERT_FALSE(inv_idle.door_raw_open);
}

void test_reed_50ms_debounce_bounce_rejection_and_no_duplicate_events_in_home(void) {
    SecurityController ctrl;
    FakeSecurityInputSource fake;
    TEST_ASSERT_TRUE(fake.begin());

    SecurityInputSampler sampler(makeImmediateSamplerConfig());

    // Initial closed/idle reading in HOME.
    fake.setRawLevels(false, false);
    SecuritySamplerStep step = sampler.poll(fake, ctrl, 100UL);
    TEST_ASSERT_TRUE(step.sample_valid);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(step.state.door_open);
    TEST_ASSERT_FALSE(step.state.alarm_on);

    // Short glitch HIGH (30ms) followed by bounce back LOW resets debounce timer.
    fake.setRawLevels(false, true);
    step = sampler.poll(fake, ctrl, 120UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(step.state.door_open);

    fake.setRawLevels(false, false);
    step = sampler.poll(fake, ctrl, 149UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(step.state.door_open);

    // Stable transition to HIGH (OPEN) at t=200ms; at t=249ms (49ms) must NOT commit yet.
    fake.setRawLevels(false, true);
    step = sampler.poll(fake, ctrl, 200UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);

    step = sampler.poll(fake, ctrl, 249UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(step.state.door_open);

    // At t=250ms (50ms) commits OPEN in HOME without activating alarm.
    step = sampler.poll(fake, ctrl, 250UL);
    TEST_ASSERT_TRUE(step.door_outcome.door_changed);
    TEST_ASSERT_TRUE(step.state.door_open);
    TEST_ASSERT_FALSE(step.any_alarm_triggered);
    TEST_ASSERT_FALSE(step.state.alarm_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TriggerSource::REED_SWITCH),
                            static_cast<uint8_t>(step.door_outcome.trigger_source));

    // Holding HIGH must NOT repeat door_changed events.
    step = sampler.poll(fake, ctrl, 300UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_TRUE(step.state.door_open);

    // Recovery back to LOW (CLOSED) requires 50ms debounce too.
    fake.setRawLevels(false, false);
    step = sampler.poll(fake, ctrl, 400UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    step = sampler.poll(fake, ctrl, 449UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_TRUE(step.state.door_open);

    step = sampler.poll(fake, ctrl, 450UL);
    TEST_ASSERT_TRUE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(step.state.door_open);
    TEST_ASSERT_FALSE(step.state.alarm_on);
}

void test_pir_rising_edge_no_repeat_recovery_and_safe_default_policy(void) {
    SecurityController ctrl;
    FakeSecurityInputSource fake;
    TEST_ASSERT_TRUE(fake.begin());

    TEST_ASSERT_FALSE(ctrl.config().pir_triggers_alarm_in_away);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(SwitchArbitrationMode::DISABLED_PENDING_APPROVAL),
        static_cast<uint8_t>(ctrl.config().switch_arbitration));

    SecurityInputSampler sampler(makeImmediateSamplerConfig());

    // 1. In HOME: PIR HIGH emits rising edge once and never activates alarm.
    fake.setRawLevels(true, false);
    SecuritySamplerStep step = sampler.poll(fake, ctrl, 100UL);
    TEST_ASSERT_TRUE(step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_TRUE(step.state.motion_detected);
    TEST_ASSERT_FALSE(step.any_alarm_triggered);
    TEST_ASSERT_FALSE(step.state.alarm_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TriggerSource::PIR),
                            static_cast<uint8_t>(step.motion_outcome.trigger_source));

    // Holding HIGH does NOT repeat motion_rising_edge.
    step = sampler.poll(fake, ctrl, 120UL);
    TEST_ASSERT_FALSE(step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_TRUE(step.state.motion_detected);
    TEST_ASSERT_FALSE(step.state.alarm_on);

    // Dropping LOW clears motion_detected cleanly.
    fake.setRawLevels(false, false);
    step = sampler.poll(fake, ctrl, 140UL);
    TEST_ASSERT_FALSE(step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_FALSE(step.state.motion_detected);
    TEST_ASSERT_FALSE(step.state.alarm_on);

    // 2. Switch to AWAY via controller API: PIR rising edge still must NOT trigger alarm
    // when pir_triggers_alarm_in_away=false.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setMode(SecurityMode::AWAY).status));

    fake.setRawLevels(true, false);
    step = sampler.poll(fake, ctrl, 160UL);
    TEST_ASSERT_TRUE(step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_TRUE(step.state.motion_detected);
    TEST_ASSERT_FALSE(step.motion_outcome.alarm_triggered);
    TEST_ASSERT_FALSE(step.any_alarm_triggered);
    TEST_ASSERT_FALSE(step.state.alarm_on);

    // Holding HIGH in AWAY still does not repeat or trigger alarm.
    step = sampler.poll(fake, ctrl, 200UL);
    TEST_ASSERT_FALSE(step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_FALSE(step.state.alarm_on);

    // Dropping LOW then rising HIGH again recovers and emits a new rising edge without alarm.
    fake.setRawLevels(false, false);
    step = sampler.poll(fake, ctrl, 220UL);
    TEST_ASSERT_FALSE(step.state.motion_detected);

    fake.setRawLevels(true, false);
    step = sampler.poll(fake, ctrl, 240UL);
    TEST_ASSERT_TRUE(step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_TRUE(step.state.motion_detected);
    TEST_ASSERT_FALSE(step.state.alarm_on);
}

void test_away_door_open_alarm_and_recovery_preserved_while_switch_disabled(void) {
    SecurityController ctrl;
    FakeSecurityInputSource fake;
    TEST_ASSERT_TRUE(fake.begin());

    SecurityInputSampler sampler(makeImmediateSamplerConfig());

    // Confirm switch arbitration remains disabled (PROP-06 pending approval).
    ctrl.updatePhysicalModeSwitch(true, 10UL);
    ctrl.updatePhysicalModeSwitch(true, 80UL);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::HOME),
                            static_cast<uint8_t>(ctrl.state().mode));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(SwitchArbitrationMode::DISABLED_PENDING_APPROVAL),
        static_cast<uint8_t>(ctrl.config().switch_arbitration));

    // Arm AWAY explicitly (simulating valid mode transition in unit test).
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setMode(SecurityMode::AWAY).status));

    // Door closed initially -> no alarm.
    fake.setRawLevels(false, false);
    SecuritySamplerStep step = sampler.poll(fake, ctrl, 100UL);
    TEST_ASSERT_FALSE(step.state.door_open);
    TEST_ASSERT_FALSE(step.state.alarm_on);

    // Door opens (reed HIGH) for 50ms in AWAY -> alarm_triggered and latched ON.
    fake.setRawLevels(false, true);
    step = sampler.poll(fake, ctrl, 200UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(step.state.alarm_on);

    step = sampler.poll(fake, ctrl, 250UL);
    TEST_ASSERT_TRUE(step.door_outcome.door_changed);
    TEST_ASSERT_TRUE(step.door_outcome.alarm_triggered);
    TEST_ASSERT_TRUE(step.any_alarm_triggered);
    TEST_ASSERT_TRUE(step.state.door_open);
    TEST_ASSERT_TRUE(step.state.alarm_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TriggerSource::EDGE_RULE),
                            static_cast<uint8_t>(step.door_outcome.trigger_source));

    // Closing door recovers door_open=false, keeps alarm latched ON, and does not re-trigger alarm.
    fake.setRawLevels(false, false);
    sampler.poll(fake, ctrl, 300UL);
    step = sampler.poll(fake, ctrl, 350UL);
    TEST_ASSERT_TRUE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(step.state.door_open);
    TEST_ASSERT_FALSE(step.any_alarm_triggered);
    TEST_ASSERT_TRUE(step.state.alarm_on);
}

void test_millis_rollover_in_sampler_interval_and_door_debounce(void) {
    SecurityController ctrl;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setMode(SecurityMode::AWAY).status));

    FakeSecurityInputSource fake;
    TEST_ASSERT_TRUE(fake.begin());

    SecurityInputSamplerConfig cfg;
    cfg.sample_interval_ms = 10UL;
    SecurityInputSampler sampler(cfg);

    // Initial poll near 32-bit wraparound must succeed immediately.
    fake.setRawLevels(false, false);
    SecuritySamplerStep step = sampler.poll(fake, ctrl, 0xFFFFFFF0UL);
    TEST_ASSERT_TRUE(step.polled);
    TEST_ASSERT_EQUAL_UINT32(1UL, fake.read_calls);

    // +9ms -> skipped (< 10ms interval).
    step = sampler.poll(fake, ctrl, 0xFFFFFFF9UL);
    TEST_ASSERT_FALSE(step.polled);
    TEST_ASSERT_EQUAL_UINT32(1UL, fake.read_calls);

    // +10ms (0xFFFFFFFA) -> polled! Door opens (reed HIGH).
    fake.setRawLevels(true, true);
    step = sampler.poll(fake, ctrl, 0xFFFFFFFAUL);
    TEST_ASSERT_TRUE(step.polled);
    TEST_ASSERT_EQUAL_UINT32(2UL, fake.read_calls);
    TEST_ASSERT_TRUE(step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(step.state.alarm_on);  // PIR does not trigger alarm; door < 50ms.

    // Across rollover at 0x00000003 (delta=9ms from 0xFFFFFFFA) -> skipped.
    step = sampler.poll(fake, ctrl, 0x00000003UL);
    TEST_ASSERT_FALSE(step.polled);
    TEST_ASSERT_EQUAL_UINT32(2UL, fake.read_calls);

    // At 0x00000004 (delta=10ms across wraparound) -> polled; door elapsed=10ms (<50ms).
    step = sampler.poll(fake, ctrl, 0x00000004UL);
    TEST_ASSERT_TRUE(step.polled);
    TEST_ASSERT_EQUAL_UINT32(3UL, fake.read_calls);
    TEST_ASSERT_FALSE(step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);

    // At 0x0000002B (delta=49ms from 0xFFFFFFFA) -> polled, still < 50ms debounce.
    step = sampler.poll(fake, ctrl, 0x0000002BUL);
    TEST_ASSERT_TRUE(step.polled);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(step.state.door_open);
    TEST_ASSERT_FALSE(step.state.alarm_on);

    // At 0x00000035 (delta=10ms from 0x0000002B, 59ms from 0xFFFFFFFA) -> commits door OPEN + alarm!
    step = sampler.poll(fake, ctrl, 0x00000035UL);
    TEST_ASSERT_TRUE(step.polled);
    TEST_ASSERT_TRUE(step.door_outcome.door_changed);
    TEST_ASSERT_TRUE(step.door_outcome.alarm_triggered);
    TEST_ASSERT_TRUE(step.any_alarm_triggered);
    TEST_ASSERT_TRUE(step.state.door_open);
    TEST_ASSERT_TRUE(step.state.alarm_on);
}

void test_invalid_sample_gap_aborts_pending_debounce_and_recovers_cleanly(void) {
    SecurityController ctrl;
    FakeSecurityInputSource fake;
    TEST_ASSERT_TRUE(fake.begin());

    SecurityInputSampler sampler(makeImmediateSamplerConfig());

    // Valid baseline CLOSED.
    fake.setRawLevels(false, false, true);
    sampler.poll(fake, ctrl, 100UL);

    // Door starts opening at t=110.
    fake.setRawLevels(false, true, true);
    SecuritySamplerStep step = sampler.poll(fake, ctrl, 110UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);

    // Source drops invalid for 60ms -> must abort pending debounce and NOT commit door_open.
    fake.setRawLevels(false, true, false);
    step = sampler.poll(fake, ctrl, 130UL);
    TEST_ASSERT_FALSE(step.sample_valid);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(ctrl.state().door_open);

    // When valid readings return at t=170, full 50ms valid window is required (170 -> 220).
    fake.setRawLevels(false, true, true);
    step = sampler.poll(fake, ctrl, 170UL);
    TEST_ASSERT_TRUE(step.sample_valid);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(ctrl.state().door_open);

    step = sampler.poll(fake, ctrl, 219UL);
    TEST_ASSERT_FALSE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(ctrl.state().door_open);

    step = sampler.poll(fake, ctrl, 220UL);
    TEST_ASSERT_TRUE(step.door_outcome.door_changed);
    TEST_ASSERT_TRUE(ctrl.state().door_open);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_safe_init_and_uninitialized_or_invalid_inputs_do_not_emit_events);
    RUN_TEST(test_reed_and_pir_polarity_mapping_and_centralized_pin_defaults);
    RUN_TEST(test_reed_50ms_debounce_bounce_rejection_and_no_duplicate_events_in_home);
    RUN_TEST(test_pir_rising_edge_no_repeat_recovery_and_safe_default_policy);
    RUN_TEST(test_away_door_open_alarm_and_recovery_preserved_while_switch_disabled);
    RUN_TEST(test_millis_rollover_in_sampler_interval_and_door_debounce);
    RUN_TEST(test_invalid_sample_gap_aborts_pending_debounce_and_recovers_cleanly);
    return UNITY_END();
}
