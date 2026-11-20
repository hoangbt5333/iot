#include <unity.h>

#include "node_security.h"

using node_sec::CommandAction;
using node_sec::CommandRejectReason;
using node_sec::CommandResult;
using node_sec::CommandStatus;
using node_sec::CommandTarget;
using node_sec::NodeBCommand;
using node_sec::SecurityController;
using node_sec::SecurityMode;
using node_sec::SecurityPolicyConfig;
using node_sec::SecurityState;
using node_sec::StepOutcome;
using node_sec::SwitchArbitrationMode;

void setUp(void) {}
void tearDown(void) {}

void test_initial_safe_state(void) {
    SecurityController ctrl;
    const SecurityState& s = ctrl.state();

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::HOME), static_cast<uint8_t>(s.mode));
    TEST_ASSERT_FALSE(s.door_open);
    TEST_ASSERT_FALSE(s.motion_detected);
    TEST_ASSERT_FALSE(s.alarm_on);
    TEST_ASSERT_EQUAL_INT16(0, s.servo_angle);
    TEST_ASSERT_FALSE(s.servo_move_requested);
    TEST_ASSERT_FALSE(ctrl.config().pir_triggers_alarm_in_away);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(SwitchArbitrationMode::DISABLED_PENDING_APPROVAL),
        static_cast<uint8_t>(ctrl.config().switch_arbitration));
}

void test_invalid_servo_config_falls_back_to_safe_defaults(void) {
    SecurityPolicyConfig bad_cfg;
    bad_cfg.servo.min_angle_deg = 120;
    bad_cfg.servo.max_angle_deg = 60;  // Invalid: min > max
    bad_cfg.servo.initial_angle_deg = 90;

    SecurityController ctrl(bad_cfg);
    TEST_ASSERT_EQUAL_INT16(0, ctrl.config().servo.min_angle_deg);
    TEST_ASSERT_EQUAL_INT16(90, ctrl.config().servo.max_angle_deg);
    TEST_ASSERT_EQUAL_INT16(0, ctrl.config().servo.initial_angle_deg);
    TEST_ASSERT_EQUAL_INT16(0, ctrl.state().servo_angle);
}

void test_door_debounce_glitches_and_millis_rollover(void) {
    SecurityController ctrl;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.setMode(SecurityMode::AWAY).status));

    // Short glitch near 32-bit millis() wraparound must NOT open door or sound alarm.
    StepOutcome out = ctrl.updateDoorInput(true, 0xFFFFFFE0UL);
    TEST_ASSERT_FALSE(out.door_changed);
    TEST_ASSERT_FALSE(out.state.door_open);
    TEST_ASSERT_FALSE(out.state.alarm_on);

    // Bounce back to false at +16ms resets debounce timer.
    out = ctrl.updateDoorInput(false, 0xFFFFFFF0UL);
    TEST_ASSERT_FALSE(out.door_changed);

    // Rising edge at 0xFFFFFFF8; at 0x00000029 elapsed is 49ms (< 50ms) -> still not committed.
    out = ctrl.updateDoorInput(true, 0xFFFFFFF8UL);
    TEST_ASSERT_FALSE(out.door_changed);
    out = ctrl.updateDoorInput(true, 0x00000029UL);
    TEST_ASSERT_FALSE(out.door_changed);
    TEST_ASSERT_FALSE(out.state.door_open);
    TEST_ASSERT_FALSE(out.state.alarm_on);

    // At 0x0000002A elapsed is exactly 50ms across wraparound -> door OPEN and alarm ON!
    out = ctrl.updateDoorInput(true, 0x0000002AUL);
    TEST_ASSERT_TRUE(out.door_changed);
    TEST_ASSERT_TRUE(out.alarm_triggered);
    TEST_ASSERT_TRUE(out.state.door_open);
    TEST_ASSERT_TRUE(out.state.alarm_on);
}

void test_home_away_door_alarm_latching_and_home_clears_alarm(void) {
    SecurityController ctrl;

    // In HOME, opening door updates door_open without activating alarm.
    StepOutcome out = ctrl.updateDoorInput(true, 100UL);
    TEST_ASSERT_FALSE(out.door_changed);
    out = ctrl.updateDoorInput(true, 150UL);
    TEST_ASSERT_TRUE(out.door_changed);
    TEST_ASSERT_TRUE(out.state.door_open);
    TEST_ASSERT_FALSE(out.state.alarm_on);

    // Close door in HOME before arming.
    ctrl.updateDoorInput(false, 200UL);
    out = ctrl.updateDoorInput(false, 250UL);
    TEST_ASSERT_TRUE(out.door_changed);
    TEST_ASSERT_FALSE(out.state.door_open);

    // Switch to AWAY; door closed -> alarm stays OFF.
    CommandResult res = ctrl.setModeFromString("AWAY");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_FALSE(res.outcome.state.alarm_on);

    // Open door in AWAY -> alarm turns ON locally (independent of MQTT).
    ctrl.updateDoorInput(true, 300UL);
    out = ctrl.updateDoorInput(true, 350UL);
    TEST_ASSERT_TRUE(out.door_changed);
    TEST_ASSERT_TRUE(out.alarm_triggered);
    TEST_ASSERT_TRUE(out.state.alarm_on);

    // Closing door while still in AWAY must keep alarm latched ON.
    ctrl.updateDoorInput(false, 400UL);
    out = ctrl.updateDoorInput(false, 450UL);
    TEST_ASSERT_TRUE(out.door_changed);
    TEST_ASSERT_FALSE(out.state.door_open);
    TEST_ASSERT_TRUE(out.state.alarm_on);

    // Switching back to HOME must clear alarm.
    res = ctrl.setModeFromString("HOME");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_TRUE(res.outcome.mode_changed);
    TEST_ASSERT_TRUE(res.outcome.alarm_cleared);
    TEST_ASSERT_FALSE(res.outcome.state.alarm_on);

    // Arming AWAY while door is already OPEN triggers alarm immediately.
    ctrl.updateDoorInput(true, 500UL);
    ctrl.updateDoorInput(true, 550UL);
    res = ctrl.setMode(SecurityMode::AWAY);
    TEST_ASSERT_TRUE(res.outcome.alarm_triggered);
    TEST_ASSERT_TRUE(res.outcome.state.alarm_on);
}

void test_pir_emits_rising_edge_only_and_never_triggers_alarm_by_default(void) {
    SecurityController ctrl;
    ctrl.setMode(SecurityMode::AWAY);

    // Rising edge LOW -> HIGH emits motion_rising_edge once, alarm remains OFF.
    StepOutcome out = ctrl.updateMotionInput(true);
    TEST_ASSERT_TRUE(out.motion_rising_edge);
    TEST_ASSERT_TRUE(out.state.motion_detected);
    TEST_ASSERT_FALSE(out.alarm_triggered);
    TEST_ASSERT_FALSE(out.state.alarm_on);

    // Holding HIGH must NOT emit repeated motion events.
    out = ctrl.updateMotionInput(true);
    TEST_ASSERT_FALSE(out.motion_rising_edge);
    TEST_ASSERT_TRUE(out.state.motion_detected);
    TEST_ASSERT_FALSE(out.state.alarm_on);

    // Dropping LOW clears motion_detected cleanly.
    out = ctrl.updateMotionInput(false);
    TEST_ASSERT_FALSE(out.motion_rising_edge);
    TEST_ASSERT_FALSE(out.state.motion_detected);
    TEST_ASSERT_FALSE(out.state.alarm_on);

    // Second rising edge emits again.
    out = ctrl.updateMotionInput(true);
    TEST_ASSERT_TRUE(out.motion_rising_edge);
    TEST_ASSERT_TRUE(out.state.motion_detected);
    TEST_ASSERT_FALSE(out.state.alarm_on);
}

void test_servo_bounds_and_configurable_limits(void) {
    SecurityController ctrl;

    // Default safe range is [0, 90].
    CommandResult res = ctrl.setServoAngle(0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_TRUE(res.outcome.servo_moved);
    TEST_ASSERT_EQUAL_INT16(0, res.outcome.state.servo_angle);

    res = ctrl.setServoAngle(90);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_EQUAL_INT16(90, res.outcome.state.servo_angle);

    // Out-of-bounds (-1, 91, 180) must be rejected and preserve previous angle (90).
    res = ctrl.setServoAngle(-1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandRejectReason::SERVO_ANGLE_OUT_OF_SAFE_RANGE),
                            static_cast<uint8_t>(res.reason));
    TEST_ASSERT_EQUAL_INT16(90, ctrl.state().servo_angle);

    res = ctrl.setServoAngle(91);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_EQUAL_INT16(90, ctrl.state().servo_angle);

    // Custom configured safe range [15, 135] with initial=20.
    SecurityPolicyConfig custom_cfg;
    custom_cfg.servo.min_angle_deg = 15;
    custom_cfg.servo.max_angle_deg = 135;
    custom_cfg.servo.initial_angle_deg = 20;
    SecurityController custom_ctrl(custom_cfg);
    TEST_ASSERT_EQUAL_INT16(20, custom_ctrl.state().servo_angle);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED),
                            static_cast<uint8_t>(custom_ctrl.setServoAngle(14).status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(custom_ctrl.setServoAngle(15).status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(custom_ctrl.setServoAngle(135).status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED),
                            static_cast<uint8_t>(custom_ctrl.setServoAngle(136).status));
    TEST_ASSERT_EQUAL_INT16(135, custom_ctrl.state().servo_angle);
}

void test_command_validation_and_alarm_silence(void) {
    SecurityController ctrl;

    // Invalid mode string & enum rejected.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED),
                            static_cast<uint8_t>(ctrl.setModeFromString(nullptr).status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED),
                            static_cast<uint8_t>(ctrl.setModeFromString("away").status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED),
                            static_cast<uint8_t>(ctrl.setMode(static_cast<SecurityMode>(42)).status));

    // Apply AWAY via command, trigger alarm via door, then silence via ALARM SILENCE command.
    NodeBCommand away_cmd;
    away_cmd.target = CommandTarget::SECURITY_MODE;
    away_cmd.action = CommandAction::SET;
    away_cmd.mode_value = SecurityMode::AWAY;
    away_cmd.has_mode_value = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(ctrl.applyCommand(away_cmd).status));

    ctrl.updateDoorInput(true, 100UL);
    ctrl.updateDoorInput(true, 150UL);
    TEST_ASSERT_TRUE(ctrl.state().alarm_on);

    NodeBCommand silence_cmd;
    silence_cmd.target = CommandTarget::ALARM;
    silence_cmd.action = CommandAction::SILENCE;
    CommandResult res = ctrl.applyCommand(silence_cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_TRUE(res.outcome.alarm_cleared);
    TEST_ASSERT_FALSE(ctrl.state().alarm_on);

    // Invalid servo/unknown commands rejected.
    NodeBCommand bad_servo;
    bad_servo.target = CommandTarget::SERVO;
    bad_servo.action = CommandAction::SET;
    bad_servo.has_int_value = false;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED),
                            static_cast<uint8_t>(ctrl.applyCommand(bad_servo).status));

    NodeBCommand unknown_cmd;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED),
                            static_cast<uint8_t>(ctrl.applyCommand(unknown_cmd).status));
}

void test_decoupled_switch_arbitration_policy(void) {
    // Default policy (DISABLED_PENDING_APPROVAL) leaves PROP-06 unresolved without hijacking mode.
    SecurityController default_ctrl;
    default_ctrl.updatePhysicalModeSwitch(true, 100UL);
    StepOutcome out = default_ctrl.updatePhysicalModeSwitch(true, 200UL);
    TEST_ASSERT_FALSE(out.mode_changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::HOME),
                            static_cast<uint8_t>(default_ctrl.state().mode));

    // When explicitly injected with EDGE_TRANSITION_LAST_WRITER_WINS, debounced switch toggles mode
    // without blocking later MQTT commands.
    SecurityPolicyConfig edge_cfg;
    edge_cfg.switch_arbitration = SwitchArbitrationMode::EDGE_TRANSITION_LAST_WRITER_WINS;
    SecurityController edge_ctrl(edge_cfg);

    edge_ctrl.updatePhysicalModeSwitch(true, 1000UL);
    out = edge_ctrl.updatePhysicalModeSwitch(true, 1050UL);
    TEST_ASSERT_TRUE(out.mode_changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::AWAY),
                            static_cast<uint8_t>(edge_ctrl.state().mode));

    // MQTT command can override back to HOME while physical switch remains steady.
    edge_ctrl.setMode(SecurityMode::HOME);
    out = edge_ctrl.updatePhysicalModeSwitch(true, 1200UL);
    TEST_ASSERT_FALSE(out.mode_changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::HOME),
                            static_cast<uint8_t>(edge_ctrl.state().mode));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_initial_safe_state);
    RUN_TEST(test_invalid_servo_config_falls_back_to_safe_defaults);
    RUN_TEST(test_door_debounce_glitches_and_millis_rollover);
    RUN_TEST(test_home_away_door_alarm_latching_and_home_clears_alarm);
    RUN_TEST(test_pir_emits_rising_edge_only_and_never_triggers_alarm_by_default);
    RUN_TEST(test_servo_bounds_and_configurable_limits);
    RUN_TEST(test_command_validation_and_alarm_silence);
    RUN_TEST(test_decoupled_switch_arbitration_policy);
    return UNITY_END();
}
