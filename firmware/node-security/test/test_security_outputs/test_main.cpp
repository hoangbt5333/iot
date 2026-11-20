#include <unity.h>

#include <cstdint>

#include "node_security.h"
#include "security_inputs.h"
#include "security_outputs.h"

using node_sec::AlarmOutputConfig;
using node_sec::AlarmOutputLevels;
using node_sec::AlarmOutputPinConfig;
using node_sec::AlarmOutputPolarityConfig;
using node_sec::AlarmOutputSync;
using node_sec::AlarmOutputSyncOutcome;
using node_sec::CommandAction;
using node_sec::CommandResult;
using node_sec::CommandStatus;
using node_sec::CommandTarget;
using node_sec::IAlarmOutputSink;
using node_sec::ISecurityInputSource;
using node_sec::NodeBCommand;
using node_sec::RawSecurityInputSample;
using node_sec::SecurityController;
using node_sec::SecurityInputSampler;
using node_sec::SecurityInputSamplerConfig;
using node_sec::SecurityMode;
using node_sec::SecuritySamplerStep;
using node_sec::SecurityState;

namespace {

class FakeAlarmOutputSink final : public IAlarmOutputSink {
public:
    static constexpr size_t kMaxTrackedPins = 40U;

    bool configureOutput(uint8_t pin, bool initial_level_high) override {
        if (fail_configure || pin >= kMaxTrackedPins) {
            return false;
        }
        ++configure_calls;
        pin_configured[pin] = true;
        pin_level_high[pin] = initial_level_high;
        last_configured_pin = pin;
        return true;
    }

    bool writeOutput(uint8_t pin, bool level_high) override {
        if (fail_write || pin >= kMaxTrackedPins || !pin_configured[pin]) {
            return false;
        }
        ++write_calls;
        ++pin_write_calls[pin];
        pin_level_high[pin] = level_high;
        return true;
    }

    bool fail_configure = false;
    bool fail_write = false;
    uint32_t configure_calls = 0;
    uint32_t write_calls = 0;
    uint8_t last_configured_pin = 0xFFU;
    bool pin_configured[kMaxTrackedPins]{};
    bool pin_level_high[kMaxTrackedPins]{};
    uint32_t pin_write_calls[kMaxTrackedPins]{};
};

class FakeSecurityInputSource final : public ISecurityInputSource {
public:
    bool begin() override {
        initialized = true;
        return true;
    }

    RawSecurityInputSample readInputs() override {
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

    bool initialized = false;
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

void test_boot_off_gpio18_gpio26_mapping_and_active_high_polarity(void) {
    AlarmOutputConfig default_cfg;
    TEST_ASSERT_EQUAL_UINT8(18U, default_cfg.pins.buzzer_pin);
    TEST_ASSERT_EQUAL_UINT8(26U, default_cfg.pins.alarm_led_pin);
    TEST_ASSERT_TRUE(default_cfg.polarity.buzzer_active_high);
    TEST_ASSERT_TRUE(default_cfg.polarity.alarm_led_active_high);

    const AlarmOutputLevels off_levels = AlarmOutputSync::mapAlarmToLevels(false, default_cfg.polarity);
    TEST_ASSERT_FALSE(off_levels.buzzer_level_high);
    TEST_ASSERT_FALSE(off_levels.alarm_led_level_high);

    const AlarmOutputLevels on_levels = AlarmOutputSync::mapAlarmToLevels(true, default_cfg.polarity);
    TEST_ASSERT_TRUE(on_levels.buzzer_level_high);
    TEST_ASSERT_TRUE(on_levels.alarm_led_level_high);

    // Sync before begin() must NOT touch GPIO sink.
    FakeAlarmOutputSink sink;
    SecurityController ctrl;
    AlarmOutputSync outputs;
    AlarmOutputSyncOutcome pre_init = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(pre_init.initialized);
    TEST_ASSERT_FALSE(pre_init.outputs_written);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.configure_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.write_calls);

    // begin() must explicitly configure GPIO18 & GPIO26 and drive both OFF (LOW).
    TEST_ASSERT_TRUE(outputs.begin(sink));
    TEST_ASSERT_TRUE(outputs.isInitialized());
    TEST_ASSERT_FALSE(outputs.lastSyncedAlarmOn());
    TEST_ASSERT_EQUAL_UINT32(2U, sink.configure_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.write_calls);
    TEST_ASSERT_TRUE(sink.pin_configured[18]);
    TEST_ASSERT_TRUE(sink.pin_configured[26]);
    TEST_ASSERT_FALSE(sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(sink.pin_level_high[26]);

    // Default HOME state (alarm_on=false) sync must NOT perform redundant GPIO writes.
    AlarmOutputSyncOutcome idle = outputs.sync(sink, ctrl);
    TEST_ASSERT_TRUE(idle.initialized);
    TEST_ASSERT_FALSE(idle.state_changed);
    TEST_ASSERT_FALSE(idle.outputs_written);
    TEST_ASSERT_FALSE(idle.alarm_on);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.write_calls);

    // Validate invalid output pin configs (input-only 34..39, flash 6..11, duplicate, invalid).
    AlarmOutputConfig bad_dup;
    bad_dup.pins.buzzer_pin = 18U;
    bad_dup.pins.alarm_led_pin = 18U;
    TEST_ASSERT_FALSE(AlarmOutputSync::isValidConfig(bad_dup));

    AlarmOutputConfig bad_input_only;
    bad_input_only.pins.buzzer_pin = 34U;
    bad_input_only.pins.alarm_led_pin = 26U;
    TEST_ASSERT_FALSE(AlarmOutputSync::isValidConfig(bad_input_only));

    AlarmOutputConfig bad_flash;
    bad_flash.pins.buzzer_pin = 18U;
    bad_flash.pins.alarm_led_pin = 6U;
    TEST_ASSERT_FALSE(AlarmOutputSync::isValidConfig(bad_flash));
}

void test_home_mode_never_activates_alarm_outputs_and_no_duplicate_writes(void) {
    SecurityController ctrl;
    FakeSecurityInputSource fake_inputs;
    TEST_ASSERT_TRUE(fake_inputs.begin());
    SecurityInputSampler sampler(makeImmediateSamplerConfig());

    FakeAlarmOutputSink sink;
    AlarmOutputSync outputs;
    TEST_ASSERT_TRUE(outputs.begin(sink));

    // In HOME, open door for 50ms + PIR HIGH must keep alarm_on=false and both outputs LOW.
    fake_inputs.setRawLevels(true, true, true);
    sampler.poll(fake_inputs, ctrl, 100UL);
    AlarmOutputSyncOutcome out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_FALSE(sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(sink.pin_level_high[26]);

    SecuritySamplerStep step = sampler.poll(fake_inputs, ctrl, 160UL);
    TEST_ASSERT_TRUE(step.door_outcome.door_changed);
    TEST_ASSERT_TRUE(step.state.door_open);
    TEST_ASSERT_FALSE(step.state.alarm_on);

    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.state_changed);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_FALSE(sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(sink.pin_level_high[26]);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.write_calls);
}

void test_away_door_open_debounce_turns_both_on_latches_on_close_and_clears_on_valid_api(void) {
    SecurityController ctrl;
    FakeSecurityInputSource fake_inputs;
    TEST_ASSERT_TRUE(fake_inputs.begin());
    SecurityInputSampler sampler(makeImmediateSamplerConfig());

    FakeAlarmOutputSink sink;
    AlarmOutputSync outputs;
    TEST_ASSERT_TRUE(outputs.begin(sink));

    // Arm AWAY explicitly via SecurityController API (runtime has no AWAY switch/MQTT yet).
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setMode(SecurityMode::AWAY).status));

    // Closed door in AWAY -> outputs stay OFF with 0 writes.
    fake_inputs.setRawLevels(false, false, true);
    sampler.poll(fake_inputs, ctrl, 100UL);
    AlarmOutputSyncOutcome out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.write_calls);

    // Door opens at t=200ms; at t=249ms (49ms < 50ms debounce) outputs must remain OFF.
    fake_inputs.setRawLevels(false, true, true);
    sampler.poll(fake_inputs, ctrl, 200UL);
    sampler.poll(fake_inputs, ctrl, 249UL);
    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_FALSE(sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(sink.pin_level_high[26]);

    // At t=250ms (50ms) door_open commits in AWAY -> alarm_on=true -> BOTH GPIO18 & GPIO26 turn HIGH.
    SecuritySamplerStep step = sampler.poll(fake_inputs, ctrl, 250UL);
    TEST_ASSERT_TRUE(step.door_outcome.door_changed);
    TEST_ASSERT_TRUE(step.door_outcome.alarm_triggered);
    TEST_ASSERT_TRUE(ctrl.state().alarm_on);

    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_TRUE(out.outputs_written);
    TEST_ASSERT_TRUE(out.alarm_on);
    TEST_ASSERT_TRUE(out.levels.buzzer_level_high);
    TEST_ASSERT_TRUE(out.levels.alarm_led_level_high);
    TEST_ASSERT_TRUE(sink.pin_level_high[18]);
    TEST_ASSERT_TRUE(sink.pin_level_high[26]);
    TEST_ASSERT_EQUAL_UINT32(2U, sink.write_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.pin_write_calls[18]);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.pin_write_calls[26]);

    // Repeated sync while alarm stays ON must NOT write GPIO again.
    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.state_changed);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_EQUAL_UINT32(2U, sink.write_calls);

    // Closing the door in AWAY keeps alarm latched ON -> outputs remain HIGH with 0 new writes.
    fake_inputs.setRawLevels(false, false, true);
    sampler.poll(fake_inputs, ctrl, 300UL);
    step = sampler.poll(fake_inputs, ctrl, 350UL);
    TEST_ASSERT_TRUE(step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(ctrl.state().door_open);
    TEST_ASSERT_TRUE(ctrl.state().alarm_on);

    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.state_changed);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_TRUE(sink.pin_level_high[18]);
    TEST_ASSERT_TRUE(sink.pin_level_high[26]);
    TEST_ASSERT_EQUAL_UINT32(2U, sink.write_calls);

    // Clearing alarm via valid SecurityController ALARM SILENCE command turns BOTH outputs OFF.
    NodeBCommand silence_cmd;
    silence_cmd.target = CommandTarget::ALARM;
    silence_cmd.action = CommandAction::SILENCE;
    CommandResult silence_res = ctrl.applyCommand(silence_cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(silence_res.status));
    TEST_ASSERT_FALSE(ctrl.state().alarm_on);

    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_TRUE(out.outputs_written);
    TEST_ASSERT_FALSE(out.alarm_on);
    TEST_ASSERT_FALSE(sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(sink.pin_level_high[26]);
    TEST_ASSERT_EQUAL_UINT32(4U, sink.write_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, sink.pin_write_calls[18]);
    TEST_ASSERT_EQUAL_UINT32(2U, sink.pin_write_calls[26]);

    // Repeated sync after clear must NOT write GPIO again.
    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_EQUAL_UINT32(4U, sink.write_calls);

    // Also verify switching back to HOME clears an active alarm and turns BOTH outputs OFF.
    fake_inputs.setRawLevels(false, true, true);
    sampler.poll(fake_inputs, ctrl, 400UL);
    sampler.poll(fake_inputs, ctrl, 450UL);
    TEST_ASSERT_TRUE(ctrl.state().alarm_on);
    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.outputs_written);
    TEST_ASSERT_TRUE(sink.pin_level_high[18]);
    TEST_ASSERT_TRUE(sink.pin_level_high[26]);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setMode(SecurityMode::HOME).status));
    TEST_ASSERT_FALSE(ctrl.state().alarm_on);
    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.outputs_written);
    TEST_ASSERT_FALSE(sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(sink.pin_level_high[26]);
}

void test_pir_does_not_activate_alarm_outputs_and_invalid_input_preserves_alarm_policy(void) {
    SecurityController ctrl;
    FakeSecurityInputSource fake_inputs;
    TEST_ASSERT_TRUE(fake_inputs.begin());
    SecurityInputSampler sampler(makeImmediateSamplerConfig());

    FakeAlarmOutputSink sink;
    AlarmOutputSync outputs;
    TEST_ASSERT_TRUE(outputs.begin(sink));

    TEST_ASSERT_FALSE(ctrl.config().pir_triggers_alarm_in_away);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setMode(SecurityMode::AWAY).status));

    // PIR HIGH in AWAY with pir_triggers_alarm_in_away=false keeps outputs OFF.
    fake_inputs.setRawLevels(true, false, true);
    SecuritySamplerStep step = sampler.poll(fake_inputs, ctrl, 100UL);
    TEST_ASSERT_TRUE(step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_FALSE(ctrl.state().alarm_on);

    AlarmOutputSyncOutcome out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_FALSE(sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(sink.pin_level_high[26]);

    // Invalid input sample (even with raw HIGH) must NOT trigger fake alarm.
    fake_inputs.setRawLevels(true, true, false);
    step = sampler.poll(fake_inputs, ctrl, 200UL);
    TEST_ASSERT_FALSE(step.sample_valid);
    TEST_ASSERT_FALSE(ctrl.state().alarm_on);
    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_FALSE(sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(sink.pin_level_high[26]);

    // Latch alarm via valid door open in AWAY.
    fake_inputs.setRawLevels(false, true, true);
    sampler.poll(fake_inputs, ctrl, 300UL);
    sampler.poll(fake_inputs, ctrl, 350UL);
    TEST_ASSERT_TRUE(ctrl.state().alarm_on);
    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.outputs_written);
    TEST_ASSERT_TRUE(sink.pin_level_high[18]);
    TEST_ASSERT_TRUE(sink.pin_level_high[26]);

    // Once alarm is latched, invalid input samples MUST preserve latched alarm ON until cleared.
    fake_inputs.setRawLevels(false, false, false);
    step = sampler.poll(fake_inputs, ctrl, 450UL);
    TEST_ASSERT_FALSE(step.sample_valid);
    TEST_ASSERT_TRUE(ctrl.state().alarm_on);
    out = outputs.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.outputs_written);
    TEST_ASSERT_TRUE(sink.pin_level_high[18]);
    TEST_ASSERT_TRUE(sink.pin_level_high[26]);
}

void test_reinitialize_drives_outputs_back_to_off_and_supports_centralized_polarity(void) {
    SecurityController ctrl;
    FakeAlarmOutputSink sink;
    AlarmOutputSync outputs;
    TEST_ASSERT_TRUE(outputs.begin(sink));

    // Turn alarm ON via valid API and sync outputs HIGH.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setAlarm(true).status));
    AlarmOutputSyncOutcome out = outputs.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.outputs_written);
    TEST_ASSERT_TRUE(sink.pin_level_high[18]);
    TEST_ASSERT_TRUE(sink.pin_level_high[26]);

    // Re-initializing outputs must immediately force both GPIO18 & GPIO26 back OFF (LOW).
    TEST_ASSERT_TRUE(outputs.begin(sink));
    TEST_ASSERT_EQUAL_UINT32(4U, sink.configure_calls);
    TEST_ASSERT_FALSE(outputs.lastSyncedAlarmOn());
    TEST_ASSERT_FALSE(sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(sink.pin_level_high[26]);

    // Verify centralized polarity override (e.g. active-LOW module) works cleanly without code edits.
    AlarmOutputConfig active_low_cfg;
    active_low_cfg.polarity.buzzer_active_high = false;
    active_low_cfg.polarity.alarm_led_active_high = false;
    AlarmOutputSync inv_outputs(active_low_cfg);
    FakeAlarmOutputSink inv_sink;
    TEST_ASSERT_TRUE(inv_outputs.begin(inv_sink));
    TEST_ASSERT_TRUE(inv_sink.pin_level_high[18]);  // OFF = HIGH for active-LOW
    TEST_ASSERT_TRUE(inv_sink.pin_level_high[26]);

    inv_outputs.sync(inv_sink, ctrl);
    TEST_ASSERT_FALSE(inv_sink.pin_level_high[18]);  // ON = LOW for active-LOW
    TEST_ASSERT_FALSE(inv_sink.pin_level_high[26]);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_boot_off_gpio18_gpio26_mapping_and_active_high_polarity);
    RUN_TEST(test_home_mode_never_activates_alarm_outputs_and_no_duplicate_writes);
    RUN_TEST(test_away_door_open_debounce_turns_both_on_latches_on_close_and_clears_on_valid_api);
    RUN_TEST(test_pir_does_not_activate_alarm_outputs_and_invalid_input_preserves_alarm_policy);
    RUN_TEST(test_reinitialize_drives_outputs_back_to_off_and_supports_centralized_polarity);
    return UNITY_END();
}
