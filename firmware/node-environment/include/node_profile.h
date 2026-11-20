#pragma once

#include <stdint.h>

#if defined(NODE_ENV_SIM) && defined(NODE_ENV_REAL)
#error "Define exactly one of NODE_ENV_SIM or NODE_ENV_REAL."
#elif !defined(NODE_ENV_SIM) && !defined(NODE_ENV_REAL)
#error "Either NODE_ENV_SIM or NODE_ENV_REAL must be defined."
#endif

namespace node_profile {

constexpr const char* kNodeId = "node-a";
constexpr const char* kNodeRole = "environment";
constexpr uint32_t kSerialBaudRate = 115200UL;
constexpr uint32_t kHeartbeatIntervalMs = 5000UL;
constexpr uint8_t kDhtDataPin = 16U;
constexpr uint32_t kDhtSampleIntervalMs = 2000UL;
constexpr uint32_t kDhtStaleTimeoutMs = 6000UL;
constexpr uint8_t kBh1750SdaPin = 21U;
constexpr uint8_t kBh1750SclPin = 22U;
constexpr uint8_t kBh1750I2cAddress = 0x23U;
constexpr uint8_t kLdrAdcPin = 34U;  // ADC1 input-only; safe with Wi-Fi (never used as output)
constexpr uint32_t kLightSampleIntervalMs = 1000UL;
constexpr uint32_t kLightStaleTimeoutMs = 5000UL;
constexpr float kLightMaxValidLux = 150000.0f;
constexpr uint8_t kLampGpioPin = 26U;  // Digital output LED
constexpr uint8_t kFanGpioPin = 25U;   // REAL: MOSFET gate/driver signal (5V fan); SIM: fan indicator LED
constexpr bool kActuatorActiveHigh = true;
constexpr uint32_t kSensorLogIntervalMs = 5000UL;

// Wi-Fi Station timing and rate-limited logging defaults (non-blocking loop tick).
constexpr uint32_t kWifiConnectTimeoutMs = 10000UL;
constexpr uint32_t kWifiBackoffInitialMs = 1000UL;
constexpr uint32_t kWifiBackoffMaxMs = 15000UL;
constexpr uint32_t kWifiLogIntervalMs = 5000UL;

#if defined(NODE_ENV_SIM)
constexpr const char* kBuildEnvName = "sim";
constexpr const char* kExpectedTempHumSensor = "DHT22";
constexpr bool kDhtIsSimulation = true;
constexpr const char* kExpectedLightSensor = "LDR";
constexpr bool kLightIsSimulation = true;
// Wokwi public simulation AP (https://docs.wokwi.com/guides/esp32-wifi); not a secret credential.
constexpr bool kWifiConfigured = true;
constexpr const char* kWifiSsid = "Wokwi-GUEST";
constexpr const char* kWifiPassword = "";
constexpr int32_t kWifiChannel = 6;
constexpr const char* kWifiConfigSource = "sim_wokwi_guest";
#elif defined(NODE_ENV_REAL)
constexpr const char* kBuildEnvName = "real";
constexpr const char* kExpectedTempHumSensor = "DHT11";
constexpr bool kDhtIsSimulation = false;
constexpr const char* kExpectedLightSensor = "BH1750";
constexpr bool kLightIsSimulation = false;
// REAL hardware must NOT hardcode user Wi-Fi credentials or auto-connect to Wokwi-GUEST.
constexpr bool kWifiConfigured = false;
constexpr const char* kWifiSsid = "";
constexpr const char* kWifiPassword = "";
constexpr int32_t kWifiChannel = 0;
constexpr const char* kWifiConfigSource = "real_unconfigured";
#endif

}  // namespace node_profile
