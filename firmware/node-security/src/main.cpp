#include <Arduino.h>
#include <stdint.h>

#include "arduino_security_inputs.h"
#include "arduino_security_outputs.h"
#include "arduino_servo_output.h"
#include "command_guard.h"
#include "node_profile.h"
#include "node_security.h"
#include "security_inputs.h"
#include "security_outputs.h"
#include "security_servo.h"
#include "utc_time.h"
#include "wifi_arduino_adapter.h"
#include "wifi_connection_manager.h"

namespace {

node_sec::SecurityController g_security;
node_sec::CommandGuard g_command_guard;
node_sec_arduino::ArduinoSecurityInputSource g_input_source(
    node_sec_arduino::ArduinoSecurityInputSource::defaultPinConfigFromProfile());
node_sec::SecurityInputSampler g_input_sampler(
    node_sec_arduino::ArduinoSecurityInputSource::defaultSamplerConfigFromProfile());
node_sec_arduino::ArduinoAlarmOutputSink g_output_sink;
node_sec::AlarmOutputSync g_alarm_outputs(
    node_sec_arduino::ArduinoAlarmOutputSink::defaultOutputConfigFromProfile());
node_sec_arduino::ArduinoServoOutputSink g_servo_sink;
node_sec::ServoOutputSync g_servo_outputs(
    node_sec_arduino::ArduinoServoOutputSink::defaultConfigFromProfile());
node_sec_arduino::ArduinoWiFiAdapter g_wifi_adapter;
node_sec::WiFiConnectionManager g_wifi_manager(
    node_sec_arduino::ArduinoWiFiAdapter::defaultConfigFromProfile());
uint32_t g_last_heartbeat_ms = 0;
uint32_t g_heartbeat_seq = 0;
bool g_last_input_fault_logged = false;
bool g_last_servo_fault_logged = false;
bool g_has_logged_wifi = false;
bool g_wifi_log_pending = false;
uint32_t g_last_wifi_log_ms = 0;

void logBootBanner(bool inputs_initialized, bool outputs_initialized, bool servo_ready) {
    const node_sec::SecurityState& sec_state = g_security.state();
    const node_sec::SecurityInputPinConfig& pin_cfg = g_input_source.pins();
    const node_sec::SecurityInputSamplerConfig& sampler_cfg = g_input_sampler.config();
    const node_sec::AlarmOutputConfig& output_cfg = g_alarm_outputs.config();
    const node_sec::AlarmOutputLevels output_levels = g_alarm_outputs.currentLevels();
    const node_sec::ServoOutputConfig& servo_cfg = g_servo_outputs.config();

    Serial.println();
    Serial.println(F("========================================"));
    Serial.print(F("node_id="));
    Serial.print(node_profile::kNodeId);
    Serial.print(F(" role="));
    Serial.print(node_profile::kNodeRole);
    Serial.print(F(" env="));
    Serial.println(node_profile::kBuildEnvName);

    Serial.print(F("planned_inputs: motion="));
    Serial.print(node_profile::kExpectedMotionSensor);
    Serial.print(F(" door="));
    Serial.print(node_profile::kExpectedDoorSensor);
    Serial.print(F(" mode_switch="));
    Serial.println(node_profile::kExpectedModeSwitch);

    Serial.print(F("inputs_gpio: initialized="));
    Serial.print(inputs_initialized ? F("true") : F("false"));
    Serial.print(F(" pir_pin="));
    Serial.print(pin_cfg.pir_pin);
    Serial.print(F(" pir_mode=INPUT pir_active="));
    Serial.print(sampler_cfg.polarity.pir_motion_active_high ? F("HIGH") : F("LOW"));
    Serial.print(F(" reed_pin="));
    Serial.print(pin_cfg.reed_pin);
    Serial.print(F(" reed_mode="));
    Serial.print(pin_cfg.reed_use_internal_pullup ? F("INPUT_PULLUP") : F("INPUT"));
    Serial.print(F(" reed_open="));
    Serial.print(sampler_cfg.polarity.reed_door_open_high ? F("HIGH") : F("LOW"));
    Serial.print(F(" reed_closed="));
    Serial.print(sampler_cfg.polarity.reed_door_open_high ? F("LOW") : F("HIGH"));
    Serial.print(F(" door_debounce_ms="));
    Serial.print(g_security.config().door_debounce_ms);
    Serial.print(F(" sample_interval_ms="));
    Serial.print(sampler_cfg.sample_interval_ms);
    Serial.println(F(" switch_gpio33=disabled_pending_approval"));

    Serial.print(F("planned_actuators: buzzer="));
    Serial.print(node_profile::kExpectedBuzzer);
    Serial.print(F(" servo="));
    Serial.println(node_profile::kExpectedServo);

    Serial.print(F("outputs_gpio: initialized="));
    Serial.print(outputs_initialized ? F("true") : F("false"));
    Serial.print(F(" buzzer_pin="));
    Serial.print(output_cfg.pins.buzzer_pin);
    Serial.print(F(" buzzer_active="));
    Serial.print(output_cfg.polarity.buzzer_active_high ? F("HIGH") : F("LOW"));
    Serial.print(F(" buzzer_level="));
    Serial.print(output_levels.buzzer_level_high ? F("HIGH") : F("LOW"));
    Serial.print(F(" led_pin="));
    Serial.print(output_cfg.pins.alarm_led_pin);
    Serial.print(F(" led_active="));
    Serial.print(output_cfg.polarity.alarm_led_active_high ? F("HIGH") : F("LOW"));
    Serial.print(F(" led_level="));
    Serial.println(output_levels.alarm_led_level_high ? F("HIGH") : F("LOW"));

    Serial.print(F("servo_gpio: ready="));
    Serial.print(servo_ready ? F("true") : F("false"));
    Serial.print(F(" attached="));
    Serial.print(g_servo_sink.isAttached() ? F("true") : F("false"));
    Serial.print(F(" pin="));
    Serial.print(servo_cfg.servo_pin);
    Serial.print(F(" freq_hz="));
    Serial.print(servo_cfg.pwm_freq_hz);
    Serial.print(F(" pulse_us="));
    Serial.print(servo_cfg.min_pulse_us);
    Serial.print(F(".."));
    Serial.print(servo_cfg.max_pulse_us);
    Serial.print(F(" safe_deg="));
    Serial.print(servo_cfg.safety.min_angle_deg);
    Serial.print(F(".."));
    Serial.print(servo_cfg.safety.max_angle_deg);
    Serial.println(F(" runtime_command_source=none_compiled_only"));

    Serial.print(F("security_safe_boot: mode="));
    Serial.print(node_sec::SecurityController::modeToString(sec_state.mode));
    Serial.print(F(" pir_alarm_away="));
    Serial.print(g_security.config().pir_triggers_alarm_in_away ? F("true") : F("false"));
    Serial.print(F(" switch_arbitration="));
    Serial.print(static_cast<uint8_t>(g_security.config().switch_arbitration));
    Serial.print(F(" alarm_on="));
    Serial.print(sec_state.alarm_on ? F("true") : F("false"));
    Serial.print(F(" servo_angle="));
    Serial.print(sec_state.servo_angle);
    Serial.print(F(" servo_move_requested="));
    Serial.println(sec_state.servo_move_requested ? F("true") : F("false"));

    Serial.print(F("command_guard: target_device="));
    Serial.print(g_command_guard.expectedDeviceId());
    Serial.print(F(" dedup_cap="));
    Serial.print(static_cast<unsigned long>(g_command_guard.dedupCapacity()));
    Serial.print(F(" dedup_storage=ram_volatile_lost_on_reboot utc_format="));
    Serial.println(node_sec::UtcParser::expectedFormat());

    const node_sec::WiFiStatusSnapshot& wifi_snap = g_wifi_manager.snapshot();
    Serial.print(F("wifi_sta: config_source="));
    Serial.print(node_profile::kWifiConfigSource);
    Serial.print(F(" configured="));
    Serial.print(wifi_snap.configured ? F("true") : F("false"));
    Serial.print(F(" state="));
    Serial.print(node_sec::WiFiConnectionManager::stateToString(wifi_snap.state));
    Serial.print(F(" channel="));
    Serial.print(node_profile::kWifiChannel);
    Serial.print(F(" timeout_ms="));
    Serial.print(node_profile::kWifiConnectTimeoutMs);
    Serial.print(F(" backoff_ms="));
    Serial.print(node_profile::kWifiBackoffInitialMs);
    Serial.print(F(".."));
    Serial.print(node_profile::kWifiBackoffMaxMs);
    Serial.println(F(" mqtt_transport=not_implemented"));

    Serial.print(F("cpu_freq_mhz="));
    Serial.print(getCpuFrequencyMhz());
    Serial.print(F(" free_heap="));
    Serial.println(ESP.getFreeHeap());
    Serial.println(F("status=inputs_outputs_ready loop_mode=non_blocking"));
    Serial.println(F("========================================"));
}

void logAlarmOutputSync(const node_sec::AlarmOutputSyncOutcome& outcome, uint32_t now_ms) {
    Serial.print(F("[output_sync] alarm_on="));
    Serial.print(outcome.alarm_on ? F("true") : F("false"));
    Serial.print(F(" buzzer_gpio18="));
    Serial.print(outcome.levels.buzzer_level_high ? F("HIGH") : F("LOW"));
    Serial.print(F(" led_gpio26="));
    Serial.print(outcome.levels.alarm_led_level_high ? F("HIGH") : F("LOW"));
    Serial.print(F(" uptime_ms="));
    Serial.println(now_ms);
}

void logServoOutputSync(const node_sec::ServoOutputSyncOutcome& outcome, uint32_t now_ms) {
    if (outcome.error != node_sec::ServoSyncError::NONE) {
        if (!g_last_servo_fault_logged) {
            g_last_servo_fault_logged = true;
            Serial.print(F("[servo_fault] error="));
            Serial.print(node_sec::ServoOutputSync::errorToString(outcome.error));
            Serial.print(F(" target_deg="));
            Serial.print(outcome.target_angle_deg);
            Serial.print(F(" failures="));
            Serial.print(outcome.consecutive_failures);
            Serial.print(F(" fault_latched="));
            Serial.print(outcome.fault_latched ? F("true") : F("false"));
            Serial.print(F(" uptime_ms="));
            Serial.println(now_ms);
        }
        return;
    }

    if (g_last_servo_fault_logged) {
        g_last_servo_fault_logged = false;
    }

    if (outcome.applied) {
        Serial.print(F("[servo_sync] gpio19_attached="));
        Serial.print(outcome.attached ? F("true") : F("false"));
        Serial.print(F(" applied_deg="));
        Serial.print(outcome.applied_angle_deg);
        Serial.print(F(" uptime_ms="));
        Serial.println(now_ms);
    }
}

void logInputEvents(const node_sec::SecuritySamplerStep& step, uint32_t now_ms) {
    if (!step.sample_valid) {
        if (!g_last_input_fault_logged) {
            g_last_input_fault_logged = true;
            Serial.print(F("[input_fault] reason=input_source_not_ready uptime_ms="));
            Serial.println(now_ms);
        }
        return;
    }

    if (g_last_input_fault_logged) {
        g_last_input_fault_logged = false;
        Serial.print(F("[input_fault] cleared=true uptime_ms="));
        Serial.println(now_ms);
    }

    if (step.door_outcome.door_changed) {
        Serial.print(F("[input_event] event_type=door_changed door_open="));
        Serial.print(step.state.door_open ? F("true") : F("false"));
        Serial.print(F(" mode="));
        Serial.print(node_sec::SecurityController::modeToString(step.state.mode));
        Serial.print(F(" alarm_on="));
        Serial.print(step.state.alarm_on ? F("true") : F("false"));
        Serial.print(F(" source=reed_switch uptime_ms="));
        Serial.println(now_ms);
    }

    if (step.motion_outcome.motion_rising_edge) {
        Serial.print(F("[input_event] event_type=motion_detected motion_detected=true mode="));
        Serial.print(node_sec::SecurityController::modeToString(step.state.mode));
        Serial.print(F(" alarm_on="));
        Serial.print(step.state.alarm_on ? F("true") : F("false"));
        Serial.print(F(" source=pir uptime_ms="));
        Serial.println(now_ms);
    }

    if (step.any_alarm_triggered) {
        Serial.print(F("[security_event] event_type=alarm_triggered door_open="));
        Serial.print(step.state.door_open ? F("true") : F("false"));
        Serial.print(F(" motion_detected="));
        Serial.print(step.state.motion_detected ? F("true") : F("false"));
        Serial.print(F(" mode="));
        Serial.print(node_sec::SecurityController::modeToString(step.state.mode));
        Serial.print(F(" source=edge_rule uptime_ms="));
        Serial.println(now_ms);
    }
}

void logWiFiStatus(uint32_t now_ms, const node_sec::WiFiStatusSnapshot& snap) {
    Serial.print(F("[wifi] state="));
    Serial.print(node_sec::WiFiConnectionManager::stateToString(snap.state));
    Serial.print(F(" err="));
    Serial.print(node_sec::WiFiConnectionManager::errorToString(snap.last_error));
    Serial.print(F(" connected="));
    Serial.print(snap.connected ? 1 : 0);
    Serial.print(F(" has_ip="));
    Serial.print(snap.has_valid_ip ? 1 : 0);
    Serial.print(F(" rssi_dbm="));
    Serial.print(static_cast<int>(snap.rssi_dbm));
    Serial.print(F(" attempts="));
    Serial.print(static_cast<unsigned long>(snap.connect_attempts));
    Serial.print(F(" failures="));
    Serial.print(static_cast<unsigned long>(snap.connect_failures));
    Serial.print(F(" backoff_ms="));
    Serial.print(static_cast<unsigned long>(snap.current_backoff_ms));
    Serial.print(F(" uptime_ms="));
    Serial.println(now_ms);
}

void logHeartbeat(uint32_t now_ms) {
    const node_sec::SecurityState& sec_state = g_security.state();
    const node_sec::WiFiStatusSnapshot& wifi_snap = g_wifi_manager.snapshot();

    ++g_heartbeat_seq;
    Serial.print(F("[heartbeat] node="));
    Serial.print(node_profile::kNodeId);
    Serial.print(F(" env="));
    Serial.print(node_profile::kBuildEnvName);
    Serial.print(F(" mode="));
    Serial.print(node_sec::SecurityController::modeToString(sec_state.mode));
    Serial.print(F(" door="));
    Serial.print(sec_state.door_open ? 1 : 0);
    Serial.print(F(" motion="));
    Serial.print(sec_state.motion_detected ? 1 : 0);
    Serial.print(F(" alarm="));
    Serial.print(sec_state.alarm_on ? 1 : 0);
    Serial.print(F(" servo_deg="));
    Serial.print(sec_state.servo_angle);
    Serial.print(F(" wifi_state="));
    Serial.print(node_sec::WiFiConnectionManager::stateToString(wifi_snap.state));
    Serial.print(F(" wifi_connected="));
    Serial.print(wifi_snap.connected ? 1 : 0);
    Serial.print(F(" wifi_has_ip="));
    Serial.print(wifi_snap.has_valid_ip ? 1 : 0);
    Serial.print(F(" mqtt_online=false telemetry_published=false seq="));
    Serial.print(g_heartbeat_seq);
    Serial.print(F(" uptime_ms="));
    Serial.print(now_ms);
    Serial.print(F(" free_heap="));
    Serial.println(ESP.getFreeHeap());
}

}  // namespace

void setup() {
    Serial.begin(node_profile::kSerialBaudRate);
    // 1. Configure GPIO18 buzzer and GPIO26 alarm LED to safe OFF/LOW prior to input setup/sampling.
    // Note: GPIO state before setup() during ESP32 ROM boot/reset is not controlled by firmware.
    const bool outputs_initialized = g_alarm_outputs.begin(g_output_sink);
    // 2. Validate servo config WITHOUT attaching GPIO19 or emitting PWM until a valid move request.
    const bool servo_ready = g_servo_outputs.begin();
    // 3. Configure GPIO27 INPUT and GPIO32 INPUT_PULLUP prior to any input read.
    const bool inputs_initialized = g_input_source.begin();
    // 4. Prepare non-blocking Wi-Fi STA manager after safe output/input initialization.
    g_wifi_manager.begin(g_wifi_adapter);
    logBootBanner(inputs_initialized, outputs_initialized, servo_ready);
    g_last_heartbeat_ms = millis();
}

void loop() {
    const uint32_t now_ms = millis();
    const node_sec::SecuritySamplerStep step =
        g_input_sampler.poll(g_input_source, g_security, now_ms);
    if (step.polled) {
        logInputEvents(step, now_ms);
    }

    const node_sec::AlarmOutputSyncOutcome output_outcome =
        g_alarm_outputs.sync(g_output_sink, g_security);
    if (output_outcome.outputs_written) {
        logAlarmOutputSync(output_outcome, now_ms);
    }

    const node_sec::ServoOutputSyncOutcome servo_outcome =
        g_servo_outputs.sync(g_servo_sink, g_security);
    if (servo_outcome.applied || servo_outcome.error != node_sec::ServoSyncError::NONE) {
        logServoOutputSync(servo_outcome, now_ms);
    }

    const node_sec::WiFiStepOutcome wifi_outcome = g_wifi_manager.tick(now_ms, g_wifi_adapter);
    if (wifi_outcome.state_changed) {
        g_wifi_log_pending = true;
    }
    if (wifi_outcome.current_state != node_sec::WiFiState::UNCONFIGURED &&
        g_wifi_log_pending &&
        node_sec::WiFiConnectionManager::shouldEmitSerialLog(
            g_has_logged_wifi, now_ms, g_last_wifi_log_ms, true,
            node_profile::kWifiLogIntervalMs)) {
        logWiFiStatus(now_ms, wifi_outcome.snapshot);
        g_has_logged_wifi = true;
        g_last_wifi_log_ms = now_ms;
        g_wifi_log_pending = false;
    }

    // Unsigned delta preserves correctness across the 32-bit millis() rollover.
    if (now_ms - g_last_heartbeat_ms >= node_profile::kHeartbeatIntervalMs) {
        g_last_heartbeat_ms = now_ms;
        logHeartbeat(now_ms);
    }
}
