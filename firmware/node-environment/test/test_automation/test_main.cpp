#include <unity.h>

#include <limits>

#include "node_automation.h"

using node_env::AutomationController;
using node_env::AutomationState;
using node_env::AutomationThresholds;
using node_env::CommandAction;
using node_env::CommandRejectReason;
using node_env::CommandResult;
using node_env::CommandStatus;
using node_env::CommandTarget;
using node_env::DeviceMode;
using node_env::NodeACommand;
using node_env::SensorHealth;
using node_env::SensorReading;

void setUp(void) {}
void tearDown(void) {}

void test_initial_safe_state(void) {
    AutomationController ctrl;
    const AutomationState& s = ctrl.state();

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceMode::AUTO), static_cast<uint8_t>(s.mode));
    TEST_ASSERT_FALSE(s.lamp_on);
    TEST_ASSERT_FALSE(s.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::UNINITIALIZED),
                            static_cast<uint8_t>(s.sensor_health));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, s.thresholds.light_on_lux);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 150.0f, s.thresholds.light_off_lux);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, s.thresholds.fan_on_temp_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 28.0f, s.thresholds.fan_off_temp_c);
}

void test_constructor_invalid_thresholds_fall_back_to_safe_defaults(void) {
    AutomationThresholds bad;
    bad.light_on_lux = 150.0f;
    bad.light_off_lux = 100.0f;  // Invalid inverted hysteresis
    bad.fan_on_temp_c = 25.0f;
    bad.fan_off_temp_c = 30.0f;

    AutomationController ctrl(bad);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, ctrl.state().thresholds.light_on_lux);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 150.0f, ctrl.state().thresholds.light_off_lux);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, ctrl.state().thresholds.fan_on_temp_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 28.0f, ctrl.state().thresholds.fan_off_temp_c);
}

void test_lamp_auto_exact_boundaries_and_hysteresis(void) {
    AutomationController ctrl;

    // 100.0 lux is inside hysteresis [100, 150]: startup OFF must stay OFF.
    AutomationState s = ctrl.evaluateSensors({25.0f, true, 100.0f, true});
    TEST_ASSERT_FALSE(s.lamp_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::OK), static_cast<uint8_t>(s.sensor_health));

    // Strictly < 100 turns lamp ON.
    s = ctrl.evaluateSensors({25.0f, true, 99.9f, true});
    TEST_ASSERT_TRUE(s.lamp_on);

    // Rising back into [100, 150] (including 100.0 and 150.0) holds lamp ON.
    s = ctrl.evaluateSensors({25.0f, true, 100.0f, true});
    TEST_ASSERT_TRUE(s.lamp_on);
    s = ctrl.evaluateSensors({25.0f, true, 125.0f, true});
    TEST_ASSERT_TRUE(s.lamp_on);
    s = ctrl.evaluateSensors({25.0f, true, 150.0f, true});
    TEST_ASSERT_TRUE(s.lamp_on);

    // Strictly > 150 turns lamp OFF.
    s = ctrl.evaluateSensors({25.0f, true, 150.1f, true});
    TEST_ASSERT_FALSE(s.lamp_on);

    // Dropping back to 150.0 and 100.0 holds lamp OFF.
    s = ctrl.evaluateSensors({25.0f, true, 150.0f, true});
    TEST_ASSERT_FALSE(s.lamp_on);
    s = ctrl.evaluateSensors({25.0f, true, 100.0f, true});
    TEST_ASSERT_FALSE(s.lamp_on);

    // Valid 0.0 lux turns lamp ON.
    s = ctrl.evaluateSensors({25.0f, true, 0.0f, true});
    TEST_ASSERT_TRUE(s.lamp_on);
}

void test_fan_auto_exact_boundaries_and_hysteresis(void) {
    AutomationController ctrl;

    // 28.0 C and 29.9 C are inside [28, 30): startup OFF must stay OFF.
    AutomationState s = ctrl.evaluateSensors({28.0f, true, 200.0f, true});
    TEST_ASSERT_FALSE(s.fan_on);
    s = ctrl.evaluateSensors({29.9f, true, 200.0f, true});
    TEST_ASSERT_FALSE(s.fan_on);

    // >= 30.0 C turns fan ON.
    s = ctrl.evaluateSensors({30.0f, true, 200.0f, true});
    TEST_ASSERT_TRUE(s.fan_on);

    // Cooling to 29.9 C and 28.0 C holds fan ON.
    s = ctrl.evaluateSensors({29.9f, true, 200.0f, true});
    TEST_ASSERT_TRUE(s.fan_on);
    s = ctrl.evaluateSensors({28.0f, true, 200.0f, true});
    TEST_ASSERT_TRUE(s.fan_on);

    // Strictly < 28.0 C turns fan OFF.
    s = ctrl.evaluateSensors({27.9f, true, 200.0f, true});
    TEST_ASSERT_FALSE(s.fan_on);

    // High temperature > 30 turns fan ON.
    s = ctrl.evaluateSensors({34.5f, true, 200.0f, true});
    TEST_ASSERT_TRUE(s.fan_on);
}

void test_manual_mode_overrides_and_blocks_auto_override(void) {
    AutomationController ctrl;

    CommandResult res = ctrl.setLampManual(true);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceMode::MANUAL), static_cast<uint8_t>(res.state.mode));
    TEST_ASSERT_TRUE(res.state.lamp_on);
    TEST_ASSERT_FALSE(res.state.fan_on);

    // Bright & hot sensor readings must NOT alter actuators in MANUAL mode.
    AutomationState s = ctrl.evaluateSensors({38.0f, true, 500.0f, true});
    TEST_ASSERT_TRUE(s.lamp_on);
    TEST_ASSERT_FALSE(s.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::OK), static_cast<uint8_t>(s.sensor_health));

    res = ctrl.setFanManual(true);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_TRUE(res.state.lamp_on);
    TEST_ASSERT_TRUE(res.state.fan_on);

    res = ctrl.setLampManual(false);
    TEST_ASSERT_FALSE(res.state.lamp_on);
    TEST_ASSERT_TRUE(res.state.fan_on);
}

void test_mode_transitions_and_invalid_mode_rejection(void) {
    AutomationController ctrl;
    ctrl.evaluateSensors({31.0f, true, 50.0f, true});
    TEST_ASSERT_TRUE(ctrl.state().lamp_on);
    TEST_ASSERT_TRUE(ctrl.state().fan_on);

    // AUTO -> MANUAL keeps current actuator states intact.
    CommandResult res = ctrl.setModeFromString("MANUAL");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_TRUE(res.state.lamp_on);
    TEST_ASSERT_TRUE(res.state.fan_on);

    // Reject invalid mode strings without altering state.
    res = ctrl.setModeFromString(nullptr);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandRejectReason::INVALID_MODE), static_cast<uint8_t>(res.reason));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceMode::MANUAL), static_cast<uint8_t>(ctrl.state().mode));

    res = ctrl.setModeFromString("auto");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));

    res = ctrl.setMode(static_cast<DeviceMode>(99));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));

    // MANUAL -> AUTO resumes AUTO control on next valid sensor evaluation.
    res = ctrl.setModeFromString("AUTO");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    AutomationState s = ctrl.evaluateSensors({25.0f, true, 300.0f, true});
    TEST_ASSERT_FALSE(s.lamp_on);
    TEST_ASSERT_FALSE(s.fan_on);
}

void test_command_validation_and_threshold_updates(void) {
    AutomationController ctrl;

    // Valid lamp command with explicit MANUAL mode.
    NodeACommand cmd;
    cmd.target = CommandTarget::LAMP;
    cmd.action = CommandAction::SET;
    cmd.bool_value = true;
    cmd.has_bool_value = true;
    cmd.mode_value = DeviceMode::MANUAL;
    cmd.has_mode_value = true;
    CommandResult res = ctrl.applyCommand(cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_TRUE(res.state.lamp_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceMode::MANUAL), static_cast<uint8_t>(res.state.mode));

    // Contradictory command: target=FAN with mode=AUTO must be rejected without changing fan.
    NodeACommand bad_mode_cmd;
    bad_mode_cmd.target = CommandTarget::FAN;
    bad_mode_cmd.action = CommandAction::SET;
    bad_mode_cmd.bool_value = true;
    bad_mode_cmd.has_bool_value = true;
    bad_mode_cmd.mode_value = DeviceMode::AUTO;
    bad_mode_cmd.has_mode_value = true;
    res = ctrl.applyCommand(bad_mode_cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandRejectReason::CONTRADICTORY_MODE),
                            static_cast<uint8_t>(res.reason));
    TEST_ASSERT_FALSE(ctrl.state().fan_on);

    // Missing bool value or unknown target rejected.
    NodeACommand missing_val;
    missing_val.target = CommandTarget::LAMP;
    missing_val.action = CommandAction::SET;
    res = ctrl.applyCommand(missing_val);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandRejectReason::INVALID_VALUE),
                            static_cast<uint8_t>(res.reason));

    NodeACommand unknown_cmd;
    res = ctrl.applyCommand(unknown_cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandRejectReason::INVALID_TARGET),
                            static_cast<uint8_t>(res.reason));

    // Valid threshold update takes effect once back in AUTO.
    NodeACommand th_cmd;
    th_cmd.target = CommandTarget::THRESHOLDS;
    th_cmd.action = CommandAction::UPDATE;
    th_cmd.has_thresholds_value = true;
    th_cmd.thresholds_value = {80.0f, 120.0f, 32.0f, 29.0f};
    res = ctrl.applyCommand(th_cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED), static_cast<uint8_t>(res.status));

    // Invalid threshold update (NaN or on >= off) must be rejected and preserve previous thresholds.
    th_cmd.thresholds_value = {120.0f, 120.0f, 32.0f, 29.0f};
    res = ctrl.applyCommand(th_cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandRejectReason::INVALID_THRESHOLDS),
                            static_cast<uint8_t>(res.reason));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 80.0f, ctrl.state().thresholds.light_on_lux);

    th_cmd.thresholds_value = {80.0f, 120.0f, std::numeric_limits<float>::quiet_NaN(), 29.0f};
    res = ctrl.applyCommand(th_cmd);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::REJECTED), static_cast<uint8_t>(res.status));
}

void test_sensor_errors_nan_inf_and_partial_faults_are_safe(void) {
    const float nan_val = std::numeric_limits<float>::quiet_NaN();
    const float inf_val = std::numeric_limits<float>::infinity();

    // Boot-time NaN/invalid readings must not turn ON lamp or fan (safe startup state).
    AutomationController ctrl;
    AutomationState s = ctrl.evaluateSensors({nan_val, false, nan_val, false});
    TEST_ASSERT_FALSE(s.lamp_on);
    TEST_ASSERT_FALSE(s.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::FAULT), static_cast<uint8_t>(s.sensor_health));

    // Move to both ON using valid data.
    s = ctrl.evaluateSensors({31.5f, true, 40.0f, true});
    TEST_ASSERT_TRUE(s.lamp_on);
    TEST_ASSERT_TRUE(s.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::OK), static_cast<uint8_t>(s.sensor_health));

    // Light sensor NaN even with light_lux_valid=true: lamp MUST hold ON, valid temp still controls fan.
    s = ctrl.evaluateSensors({25.0f, true, nan_val, true});
    TEST_ASSERT_TRUE(s.lamp_on);
    TEST_ASSERT_FALSE(s.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::DEGRADED),
                            static_cast<uint8_t>(s.sensor_health));

    // Negative or Inf lux is invalid: lamp holds ON; temp fault (valid=false with 35C) leaves fan OFF.
    s = ctrl.evaluateSensors({35.0f, false, -5.0f, true});
    TEST_ASSERT_TRUE(s.lamp_on);
    TEST_ASSERT_FALSE(s.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::FAULT), static_cast<uint8_t>(s.sensor_health));

    // Inf temperature or out-of-range temperature leaves fan unchanged while valid high lux turns lamp OFF.
    s = ctrl.evaluateSensors({inf_val, true, 250.0f, true});
    TEST_ASSERT_FALSE(s.lamp_on);
    TEST_ASSERT_FALSE(s.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::DEGRADED),
                            static_cast<uint8_t>(s.sensor_health));

    s = ctrl.evaluateSensors({-60.0f, true, 250.0f, true});
    TEST_ASSERT_FALSE(s.fan_on);
    s = ctrl.evaluateSensors({130.0f, true, 250.0f, true});
    TEST_ASSERT_FALSE(s.fan_on);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_initial_safe_state);
    RUN_TEST(test_constructor_invalid_thresholds_fall_back_to_safe_defaults);
    RUN_TEST(test_lamp_auto_exact_boundaries_and_hysteresis);
    RUN_TEST(test_fan_auto_exact_boundaries_and_hysteresis);
    RUN_TEST(test_manual_mode_overrides_and_blocks_auto_override);
    RUN_TEST(test_mode_transitions_and_invalid_mode_rejection);
    RUN_TEST(test_command_validation_and_threshold_updates);
    RUN_TEST(test_sensor_errors_nan_inf_and_partial_faults_are_safe);
    return UNITY_END();
}
