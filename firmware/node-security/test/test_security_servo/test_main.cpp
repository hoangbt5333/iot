#include <unity.h>

#include <cstdint>

#include "node_security.h"
#include "security_inputs.h"
#include "security_outputs.h"
#include "security_servo.h"

using node_sec::AlarmOutputSync;
using node_sec::CommandAction;
using node_sec::CommandRejectReason;
using node_sec::CommandResult;
using node_sec::CommandStatus;
using node_sec::CommandTarget;
using node_sec::IAlarmOutputSink;
using node_sec::ISecurityInputSource;
using node_sec::IServoOutputSink;
using node_sec::NodeBCommand;
using node_sec::RawSecurityInputSample;
using node_sec::SecurityController;
using node_sec::SecurityInputSampler;
using node_sec::SecurityInputSamplerConfig;
using node_sec::SecurityMode;
using node_sec::SecurityState;
using node_sec::ServoOutputConfig;
using node_sec::ServoOutputSync;
using node_sec::ServoOutputSyncOutcome;
using node_sec::ServoSafetyConfig;
using node_sec::ServoSyncError;
using node_sec::SwitchArbitrationMode;

namespace {

class FakeServoOutputSink final : public IServoOutputSink {
public:
    bool isAttached() const override { return attached; }

    bool attach(uint8_t pin,
                uint16_t pwm_freq_hz,
                uint16_t min_pulse_us,
                uint16_t max_pulse_us) override {
        ++attach_calls;
        last_pin = pin;
        last_freq_hz = pwm_freq_hz;
        last_min_pulse_us = min_pulse_us;
        last_max_pulse_us = max_pulse_us;
        if (fail_attach) {
            attached = false;
            return false;
        }
        attached = true;
        return true;
    }

    bool writeAngle(int16_t angle_deg) override {
        ++write_calls;
        if (fail_write || !attached) {
            return false;
        }
        last_written_angle_deg = angle_deg;
        return true;
    }

    void detach() override {
        ++detach_calls;
        attached = false;
    }

    bool attached = false;
    bool fail_attach = false;
    bool fail_write = false;
    uint32_t attach_calls = 0;
    uint32_t write_calls = 0;
    uint32_t detach_calls = 0;
    uint8_t last_pin = 0xFFU;
    uint16_t last_freq_hz = 0U;
    uint16_t last_min_pulse_us = 0U;
    uint16_t last_max_pulse_us = 0U;
    int16_t last_written_angle_deg = -1;
};

class FakeAlarmOutputSink final : public IAlarmOutputSink {
public:
    static constexpr size_t kMaxPins = 40U;

    bool configureOutput(uint8_t pin, bool initial_level_high) override {
        if (pin >= kMaxPins) {
            return false;
        }
        pin_level_high[pin] = initial_level_high;
        return true;
    }

    bool writeOutput(uint8_t pin, bool level_high) override {
        if (pin >= kMaxPins) {
            return false;
        }
        pin_level_high[pin] = level_high;
        return true;
    }

    bool pin_level_high[kMaxPins]{};
};

class FakeSecurityInputSource final : public ISecurityInputSource {
public:
    bool begin() override {
        initialized = true;
        return true;
    }

    RawSecurityInputSample readInputs() override {
        return initialized ? sample : RawSecurityInputSample{};
    }

    void setRawLevels(bool pir_high, bool reed_high, bool valid = true) {
        sample.valid = valid;
        sample.pir_level_high = pir_high;
        sample.reed_level_high = reed_high;
    }

    bool initialized = false;
    RawSecurityInputSample sample{};
};

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_boot_never_attaches_or_emits_pwm_without_valid_request(void) {
    SecurityController ctrl;
    FakeServoOutputSink sink;
    ServoOutputSync servo_sync;

    TEST_ASSERT_TRUE(servo_sync.begin());
    TEST_ASSERT_TRUE(servo_sync.isInitialized());
    TEST_ASSERT_FALSE(servo_sync.hasAppliedMove());
    TEST_ASSERT_FALSE(sink.isAttached());
    TEST_ASSERT_EQUAL_UINT32(0U, sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.write_calls);

    // Repeated loop syncs without servo_move_requested must NEVER attach or emit PWM.
    for (int i = 0; i < 10; ++i) {
        const ServoOutputSyncOutcome out = servo_sync.sync(sink, ctrl);
        TEST_ASSERT_TRUE(out.initialized);
        TEST_ASSERT_FALSE(out.attached);
        TEST_ASSERT_FALSE(out.move_attempted);
        TEST_ASSERT_FALSE(out.applied);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ServoSyncError::NONE),
                                static_cast<uint8_t>(out.error));
    }
    TEST_ASSERT_FALSE(ctrl.state().servo_move_requested);
    TEST_ASSERT_EQUAL_INT16(0, ctrl.state().servo_angle);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.write_calls);
}

void test_valid_angles_0_45_90_including_initial_zero_request_and_no_repeat_writes(void) {
    SecurityController ctrl;
    FakeServoOutputSink sink;
    ServoOutputSync servo_sync;
    TEST_ASSERT_TRUE(servo_sync.begin());

    // Default config must pin GPIO19, 50Hz, 500..2400us, and [0, 90] deg safety bounds.
    TEST_ASSERT_EQUAL_UINT8(19U, servo_sync.config().servo_pin);
    TEST_ASSERT_EQUAL_UINT16(50U, servo_sync.config().pwm_freq_hz);
    TEST_ASSERT_EQUAL_UINT16(500U, servo_sync.config().min_pulse_us);
    TEST_ASSERT_EQUAL_UINT16(2400U, servo_sync.config().max_pulse_us);
    TEST_ASSERT_EQUAL_INT16(0, servo_sync.config().safety.min_angle_deg);
    TEST_ASSERT_EQUAL_INT16(90, servo_sync.config().safety.max_angle_deg);

    // First valid request at 0 deg (matching boot default 0) MUST attach and write once.
    CommandResult cmd_res = ctrl.setServoAngle(0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(cmd_res.status));
    TEST_ASSERT_TRUE(ctrl.state().servo_move_requested);

    ServoOutputSyncOutcome out = servo_sync.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.attached);
    TEST_ASSERT_TRUE(out.move_attempted);
    TEST_ASSERT_TRUE(out.applied);
    TEST_ASSERT_EQUAL_INT16(0, out.applied_angle_deg);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.write_calls);
    TEST_ASSERT_EQUAL_UINT8(19U, sink.last_pin);
    TEST_ASSERT_EQUAL_UINT16(50U, sink.last_freq_hz);
    TEST_ASSERT_EQUAL_UINT16(500U, sink.last_min_pulse_us);
    TEST_ASSERT_EQUAL_UINT16(2400U, sink.last_max_pulse_us);
    TEST_ASSERT_EQUAL_INT16(0, sink.last_written_angle_deg);
    // ServoOutputSync must NOT mutate/clear SecurityController sticky flag.
    TEST_ASSERT_TRUE(ctrl.state().servo_move_requested);

    // Subsequent loop ticks with sticky servo_move_requested=true must NOT repeat writeAngle().
    for (int i = 0; i < 5; ++i) {
        out = servo_sync.sync(sink, ctrl);
        TEST_ASSERT_FALSE(out.move_attempted);
        TEST_ASSERT_FALSE(out.applied);
    }
    TEST_ASSERT_EQUAL_UINT32(1U, sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.write_calls);

    // Move to 45 degrees -> writes once without re-attaching.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setServoAngle(45).status));
    out = servo_sync.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.move_attempted);
    TEST_ASSERT_TRUE(out.applied);
    TEST_ASSERT_EQUAL_INT16(45, sink.last_written_angle_deg);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, sink.write_calls);

    out = servo_sync.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.move_attempted);
    TEST_ASSERT_EQUAL_UINT32(2U, sink.write_calls);

    // Move to 90 degrees -> writes once.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setServoAngle(90).status));
    out = servo_sync.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.applied);
    TEST_ASSERT_EQUAL_INT16(90, sink.last_written_angle_deg);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(3U, sink.write_calls);
}

void test_rejected_out_of_range_angles_never_attach_or_emit_pwm(void) {
    SecurityController ctrl;
    FakeServoOutputSink sink;
    ServoOutputSync servo_sync;
    TEST_ASSERT_TRUE(servo_sync.begin());

    const int32_t bad_angles[] = {-1, 91, 180};
    for (size_t i = 0; i < sizeof(bad_angles) / sizeof(bad_angles[0]); ++i) {
        CommandResult res = ctrl.setServoAngle(bad_angles[i]);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED),
                                static_cast<uint8_t>(res.status));
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(CommandRejectReason::SERVO_ANGLE_OUT_OF_SAFE_RANGE),
            static_cast<uint8_t>(res.reason));

        ServoOutputSyncOutcome out = servo_sync.sync(sink, ctrl);
        TEST_ASSERT_FALSE(out.move_attempted);
        TEST_ASSERT_FALSE(out.applied);
        TEST_ASSERT_FALSE(sink.isAttached());
    }
    TEST_ASSERT_EQUAL_UINT32(0U, sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.write_calls);

    // Defense-in-depth: even if SecurityState carried an out-of-range angle, syncState blocks it.
    SecurityState forged_state;
    forged_state.servo_move_requested = true;
    forged_state.servo_angle = 91;
    ServoOutputSyncOutcome blocked = servo_sync.syncState(sink, forged_state);
    TEST_ASSERT_FALSE(blocked.applied);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ServoSyncError::ANGLE_OUT_OF_SAFE_RANGE),
                            static_cast<uint8_t>(blocked.error));
    TEST_ASSERT_EQUAL_UINT32(0U, sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.write_calls);
}

void test_attach_and_write_failures_bounded_retries_and_recovery(void) {
    SecurityController ctrl;
    FakeServoOutputSink sink;
    ServoOutputSync servo_sync;
    TEST_ASSERT_TRUE(servo_sync.begin());

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setServoAngle(45).status));

    // 1. Transient attach failure (2 failures < max 3) recovers automatically on 3rd attempt.
    sink.fail_attach = true;
    ServoOutputSyncOutcome out = servo_sync.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.applied);
    TEST_ASSERT_FALSE(servo_sync.hasAppliedMove());
    TEST_ASSERT_FALSE(out.fault_latched);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ServoSyncError::ATTACH_FAILED),
                            static_cast<uint8_t>(out.error));
    TEST_ASSERT_EQUAL_UINT8(1U, out.consecutive_failures);

    out = servo_sync.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.applied);
    TEST_ASSERT_FALSE(out.fault_latched);
    TEST_ASSERT_EQUAL_UINT8(2U, out.consecutive_failures);

    sink.fail_attach = false;
    out = servo_sync.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.applied);
    TEST_ASSERT_TRUE(servo_sync.hasAppliedMove());
    TEST_ASSERT_FALSE(out.fault_latched);
    TEST_ASSERT_EQUAL_INT16(45, sink.last_written_angle_deg);
    TEST_ASSERT_EQUAL_UINT8(0U, out.consecutive_failures);

    // 2. Persistent write failure hits retry limit (3), latches fault, stops unbounded retries,
    // and recovers cleanly after clearFault().
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setServoAngle(90).status));
    sink.fail_write = true;
    const uint32_t writes_before = sink.write_calls;

    for (uint8_t attempt = 1U; attempt <= 3U; ++attempt) {
        out = servo_sync.sync(sink, ctrl);
        TEST_ASSERT_FALSE(out.applied);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ServoSyncError::WRITE_FAILED),
                                static_cast<uint8_t>(out.error));
        TEST_ASSERT_EQUAL_UINT8(attempt, out.consecutive_failures);
    }
    TEST_ASSERT_TRUE(out.fault_latched);
    TEST_ASSERT_EQUAL_INT16(45, servo_sync.lastAppliedAngleDeg());
    TEST_ASSERT_EQUAL_UINT32(writes_before + 3U, sink.write_calls);

    // Subsequent loop ticks must NOT keep retrying indefinitely.
    for (int i = 0; i < 5; ++i) {
        out = servo_sync.sync(sink, ctrl);
        TEST_ASSERT_TRUE(out.fault_latched);
        TEST_ASSERT_TRUE(out.retry_suppressed);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ServoSyncError::FAULT_LATCHED_RETRY_LIMIT),
                                static_cast<uint8_t>(out.error));
    }
    TEST_ASSERT_EQUAL_UINT32(writes_before + 3U, sink.write_calls);

    // After fault is cleared and sink recovers, sync applies the pending target angle 90.
    sink.fail_write = false;
    servo_sync.clearFault();
    out = servo_sync.sync(sink, ctrl);
    TEST_ASSERT_TRUE(out.applied);
    TEST_ASSERT_FALSE(out.fault_latched);
    TEST_ASSERT_EQUAL_INT16(90, servo_sync.lastAppliedAngleDeg());
    TEST_ASSERT_EQUAL_INT16(90, sink.last_written_angle_deg);
}

void test_servo_never_moves_on_pir_door_or_alarm_and_preserves_buzzer_led_and_policies(void) {
    SecurityController ctrl;
    FakeSecurityInputSource fake_inputs;
    TEST_ASSERT_TRUE(fake_inputs.begin());
    SecurityInputSamplerConfig sampler_cfg;
    sampler_cfg.sample_interval_ms = 0UL;
    SecurityInputSampler sampler(sampler_cfg);

    FakeAlarmOutputSink alarm_sink;
    AlarmOutputSync alarm_outputs;
    TEST_ASSERT_TRUE(alarm_outputs.begin(alarm_sink));

    FakeServoOutputSink servo_sink;
    ServoOutputSync servo_sync;
    TEST_ASSERT_TRUE(servo_sync.begin());

    // Verify unapproved policies stay untouched.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::HOME),
                            static_cast<uint8_t>(ctrl.state().mode));
    TEST_ASSERT_FALSE(ctrl.config().pir_triggers_alarm_in_away);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(SwitchArbitrationMode::DISABLED_PENDING_APPROVAL),
        static_cast<uint8_t>(ctrl.config().switch_arbitration));

    // PIR + Door open in HOME -> no alarm, no servo attach/move.
    fake_inputs.setRawLevels(true, true, true);
    sampler.poll(fake_inputs, ctrl, 100UL);
    sampler.poll(fake_inputs, ctrl, 150UL);
    alarm_outputs.sync(alarm_sink, ctrl);
    servo_sync.sync(servo_sink, ctrl);
    TEST_ASSERT_FALSE(ctrl.state().alarm_on);
    TEST_ASSERT_FALSE(alarm_sink.pin_level_high[18]);
    TEST_ASSERT_FALSE(alarm_sink.pin_level_high[26]);
    TEST_ASSERT_EQUAL_UINT32(0U, servo_sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, servo_sink.write_calls);

    // Close door, arm AWAY, open door -> GPIO18 buzzer & GPIO26 LED turn HIGH, servo stays untouched!
    fake_inputs.setRawLevels(false, false, true);
    sampler.poll(fake_inputs, ctrl, 200UL);
    sampler.poll(fake_inputs, ctrl, 250UL);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setMode(SecurityMode::AWAY).status));

    fake_inputs.setRawLevels(true, true, true);
    sampler.poll(fake_inputs, ctrl, 300UL);
    sampler.poll(fake_inputs, ctrl, 350UL);
    alarm_outputs.sync(alarm_sink, ctrl);
    ServoOutputSyncOutcome servo_out = servo_sync.sync(servo_sink, ctrl);

    TEST_ASSERT_TRUE(ctrl.state().alarm_on);
    TEST_ASSERT_TRUE(alarm_sink.pin_level_high[18]);
    TEST_ASSERT_TRUE(alarm_sink.pin_level_high[26]);
    TEST_ASSERT_FALSE(ctrl.state().servo_move_requested);
    TEST_ASSERT_FALSE(servo_out.attached);
    TEST_ASSERT_FALSE(servo_out.move_attempted);
    TEST_ASSERT_EQUAL_UINT32(0U, servo_sink.attach_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, servo_sink.write_calls);
}

void test_invalid_servo_output_config_and_pin_validation(void) {
    TEST_ASSERT_TRUE(ServoOutputSync::isValidServoPwmPin(19U));
    TEST_ASSERT_FALSE(ServoOutputSync::isValidServoPwmPin(6U));
    TEST_ASSERT_FALSE(ServoOutputSync::isValidServoPwmPin(34U));
    TEST_ASSERT_FALSE(ServoOutputSync::isValidServoPwmPin(ServoOutputConfig::kInvalidPin));

    ServoOutputConfig bad_pin;
    bad_pin.servo_pin = 34U;
    TEST_ASSERT_FALSE(ServoOutputSync::isValidConfig(bad_pin));
    ServoOutputSync bad_sync(bad_pin);
    TEST_ASSERT_FALSE(bad_sync.begin());

    FakeServoOutputSink sink;
    SecurityController ctrl;
    ctrl.setServoAngle(45);
    ServoOutputSyncOutcome out = bad_sync.sync(sink, ctrl);
    TEST_ASSERT_FALSE(out.initialized);
    TEST_ASSERT_FALSE(out.applied);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ServoSyncError::INVALID_CONFIG),
                            static_cast<uint8_t>(out.error));
    TEST_ASSERT_EQUAL_UINT32(0U, sink.attach_calls);

    ServoOutputConfig bad_pulse;
    bad_pulse.min_pulse_us = 400U;  // Below ESP32Servo MIN_PULSE_WIDTH (500us)
    TEST_ASSERT_FALSE(ServoOutputSync::isValidConfig(bad_pulse));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_boot_never_attaches_or_emits_pwm_without_valid_request);
    RUN_TEST(test_valid_angles_0_45_90_including_initial_zero_request_and_no_repeat_writes);
    RUN_TEST(test_rejected_out_of_range_angles_never_attach_or_emit_pwm);
    RUN_TEST(test_attach_and_write_failures_bounded_retries_and_recovery);
    RUN_TEST(test_servo_never_moves_on_pir_door_or_alarm_and_preserves_buzzer_led_and_policies);
    RUN_TEST(test_invalid_servo_output_config_and_pin_validation);
    return UNITY_END();
}
