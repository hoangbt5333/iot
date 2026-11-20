#pragma once

#include <stdint.h>

#if defined(NODE_ENV_SIM) && defined(NODE_ENV_REAL)
#error "Define exactly one of NODE_ENV_SIM or NODE_ENV_REAL."
#elif !defined(NODE_ENV_SIM) && !defined(NODE_ENV_REAL)
#error "Either NODE_ENV_SIM or NODE_ENV_REAL must be defined."
#endif

namespace node_profile {

constexpr const char* kNodeId = "node-b";
constexpr const char* kNodeRole = "security";
constexpr uint32_t kSerialBaudRate = 115200UL;
constexpr uint32_t kHeartbeatIntervalMs = 5000UL;

// Digital input pins (ESP-WROOM-32 DevKit; 3.3V logic only, NEVER apply 5V directly to GPIO).
// Physical HOME/AWAY switch (planned GPIO33) remains disabled until switch arbitration is approved.
constexpr uint8_t kPirInputPin = 27U;
constexpr uint8_t kReedSwitchInputPin = 32U;
constexpr uint32_t kInputSampleIntervalMs = 10UL;

// Digital alarm output pins (ESP-WROOM-32 DevKit; 3.3V logic only).
// Physical HOME/AWAY switch (planned GPIO33) remains disabled until switch arbitration is approved.
constexpr uint8_t kBuzzerOutputPin = 18U;
constexpr uint8_t kAlarmLedOutputPin = 26U;

// Servo PWM output configuration (ESP32 GPIO19, 50Hz standard hobby servo period).
// Pulse range [500, 2400] us follows ESP32Servo (MIN_PULSE_WIDTH=500, DEFAULT_uS_HIGH=2400)
// and typical SG90 datasheet bounds; verify physical servo end-stops before mechanical coupling.
// REAL wiring: SG90 requires dedicated 5V power with common GND; GPIO19 provides 3.3V PWM signal only.
constexpr uint8_t kServoPwmPin = 19U;
constexpr uint16_t kServoPwmFreqHz = 50U;
constexpr uint16_t kServoMinPulseUs = 500U;
constexpr uint16_t kServoMaxPulseUs = 2400U;
constexpr int16_t kServoMinAngleDeg = 0;
constexpr int16_t kServoMaxAngleDeg = 90;
constexpr int16_t kServoInitialAngleDeg = 0;

// Wi-Fi Station timing and rate-limited logging defaults (non-blocking loop tick).
constexpr uint32_t kWifiConnectTimeoutMs = 10000UL;
constexpr uint32_t kWifiBackoffInitialMs = 1000UL;
constexpr uint32_t kWifiBackoffMaxMs = 15000UL;
constexpr uint32_t kWifiLogIntervalMs = 5000UL;

#if defined(NODE_ENV_SIM)
constexpr const char* kBuildEnvName = "sim";
constexpr const char* kExpectedMotionSensor = "PIR";
constexpr const char* kExpectedDoorSensor = "SLIDE_SWITCH_REED_SIM";
constexpr const char* kExpectedModeSwitch = "NONE";
constexpr const char* kExpectedBuzzer = "BUZZER";
constexpr const char* kExpectedServo = "SERVO";
// Centralized input polarity for SIM:
// - PIR digital output: HIGH => motion detected.
// - Door switch to GND with INPUT_PULLUP: LOW => CLOSED (door_open=false), HIGH => OPEN (door_open=true).
constexpr bool kPirMotionActiveHigh = true;
constexpr bool kReedDoorOpenHigh = true;
constexpr bool kReedUseInternalPullup = true;
// Centralized output polarity for SIM (ACTIVE_HIGH assumption; GPIO18 is alarm logic signal only):
constexpr bool kBuzzerActiveHigh = true;
constexpr bool kAlarmLedActiveHigh = true;
// Wokwi public simulation AP (https://docs.wokwi.com/guides/esp32-wifi); not a secret credential.
constexpr bool kWifiConfigured = true;
constexpr const char* kWifiSsid = "Wokwi-GUEST";
constexpr const char* kWifiPassword = "";
constexpr int32_t kWifiChannel = 6;
constexpr const char* kWifiConfigSource = "sim_wokwi_guest";
#elif defined(NODE_ENV_REAL)
constexpr const char* kBuildEnvName = "real";
constexpr const char* kExpectedMotionSensor = "PIR_HC_SR501";
constexpr const char* kExpectedDoorSensor = "REED_SWITCH";
constexpr const char* kExpectedModeSwitch = "HOME_AWAY_SWITCH";
constexpr const char* kExpectedBuzzer = "BUZZER";
constexpr const char* kExpectedServo = "SG90";
// Centralized input polarity for REAL (re-verify against physical HC-SR501 / reed module before wiring):
// - HC-SR501 OUT (3.3V logic): HIGH => motion detected.
// - Proposed reed switch wired GPIO32 <-> GND with INPUT_PULLUP: LOW => CLOSED, HIGH => OPEN.
constexpr bool kPirMotionActiveHigh = true;
constexpr bool kReedDoorOpenHigh = true;
constexpr bool kReedUseInternalPullup = true;
// Centralized output polarity for REAL (ACTIVE_HIGH assumption; verify physical active buzzer/3.3V driver & LED wiring):
// - NEVER drive a 5V or over-current buzzer directly from GPIO18; use active buzzer / 3.3V-compatible driver.
constexpr bool kBuzzerActiveHigh = true;
constexpr bool kAlarmLedActiveHigh = true;
// REAL hardware must NOT hardcode user Wi-Fi credentials or auto-connect to Wokwi-GUEST.
constexpr bool kWifiConfigured = false;
constexpr const char* kWifiSsid = "";
constexpr const char* kWifiPassword = "";
constexpr int32_t kWifiChannel = 0;
constexpr const char* kWifiConfigSource = "real_unconfigured";
#endif

}  // namespace node_profile
