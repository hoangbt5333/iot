#include <Arduino.h>
#include <DHT.h>
#include <stdint.h>

#include "command_guard.h"
#include "dht_arduino_driver.h"
#include "gpio_arduino_sink.h"
#include "light_arduino_driver.h"
#include "light_sensor.h"
#include "node_actuators.h"
#include "node_automation.h"
#include "node_profile.h"
#include "temp_humidity_sensor.h"
#include "utc_time.h"
#include "wifi_arduino_adapter.h"
#include "wifi_connection_manager.h"

namespace {

#if defined(NODE_ENV_SIM)
constexpr uint8_t kAdafruitDhtType = DHT22;
constexpr node_env::DhtSensorKind kCompileTimeDhtKind = node_env::DhtSensorKind::DHT22;
constexpr node_env::LightSensorKind kCompileTimeLightKind = node_env::LightSensorKind::LDR_ADC_SIM;
constexpr const char* kFanOutputRole = "sim_fan_indicator_led";
#elif defined(NODE_ENV_REAL)
constexpr uint8_t kAdafruitDhtType = DHT11;
constexpr node_env::DhtSensorKind kCompileTimeDhtKind = node_env::DhtSensorKind::DHT11;
constexpr node_env::LightSensorKind kCompileTimeLightKind = node_env::LightSensorKind::BH1750;
constexpr const char* kFanOutputRole = "mosfet_gate_driver_5v_fan";
#endif

constexpr node_env::ActuatorPolarity kActuatorPolarity =
    node_profile::kActuatorActiveHigh ? node_env::ActuatorPolarity::ACTIVE_HIGH
                                      : node_env::ActuatorPolarity::ACTIVE_LOW;

node_env::AutomationController g_automation;
node_env::CommandGuard g_command_guard;
node_env::ArduinoGpioSink g_gpio_sink;
node_env::NodeActuatorSync g_actuator_sync(node_env::ActuatorPinConfig{
    node_profile::kLampGpioPin,
    node_profile::kFanGpioPin,
    kActuatorPolarity,
    kActuatorPolarity,
});
node_env::DhtArduinoDriver g_dht_driver(node_profile::kDhtDataPin, kAdafruitDhtType);
node_env::TempHumiditySensor g_temp_hum_sensor(node_env::TempHumidityConfig{
    kCompileTimeDhtKind,
    node_profile::kDhtSampleIntervalMs,
    node_profile::kDhtStaleTimeoutMs,
});

#if defined(NODE_ENV_SIM)
node_env::LdrAdcArduinoDriver g_light_driver(node_profile::kLdrAdcPin,
                                             node_profile::kLightMaxValidLux);
#elif defined(NODE_ENV_REAL)
node_env::Bh1750ArduinoDriver g_light_driver(node_profile::kBh1750SdaPin,
                                             node_profile::kBh1750SclPin,
                                             node_profile::kBh1750I2cAddress);
#endif
node_env::LightSensor g_light_sensor(node_env::LightSensorConfig{
    kCompileTimeLightKind,
    node_profile::kLightSampleIntervalMs,
    node_profile::kLightStaleTimeoutMs,
    node_profile::kLightMaxValidLux,
});
node_env::ArduinoWiFiAdapter g_wifi_adapter;
node_env::WiFiConnectionManager g_wifi_manager(
    node_env::ArduinoWiFiAdapter::defaultConfigFromProfile());

bool g_light_begin_ok = false;
uint32_t g_last_heartbeat_ms = 0;
uint32_t g_heartbeat_seq = 0;
bool g_has_logged_dht = false;
uint32_t g_last_dht_log_ms = 0;
bool g_has_logged_light = false;
uint32_t g_last_light_log_ms = 0;
bool g_has_logged_wifi = false;
bool g_wifi_log_pending = false;
uint32_t g_last_wifi_log_ms = 0;

void logBootBanner() {
    const node_env::AutomationState& auto_state = g_automation.state();
    const node_env::ActuatorOutputs& act_out = g_actuator_sync.outputs();

    Serial.println();
    Serial.println(F("========================================"));
    Serial.print(F("node_id="));
    Serial.print(node_profile::kNodeId);
    Serial.print(F(" role="));
    Serial.print(node_profile::kNodeRole);
    Serial.print(F(" env="));
    Serial.println(node_profile::kBuildEnvName);

    Serial.print(F("planned_sensors: temp_hum="));
    Serial.print(node_profile::kExpectedTempHumSensor);
    Serial.print(F(" light="));
    Serial.println(node_profile::kExpectedLightSensor);

    Serial.print(F("dht_driver: sensor="));
    Serial.print(node_env::TempHumiditySensor::sensorKindToString(kCompileTimeDhtKind));
    Serial.print(F(" sim="));
    Serial.print(node_profile::kDhtIsSimulation ? F("true") : F("false"));
    Serial.print(F(" gpio="));
    Serial.print(node_profile::kDhtDataPin);
    Serial.print(F(" interval_ms="));
    Serial.print(g_temp_hum_sensor.config().sample_interval_ms);
    Serial.print(F(" stale_ms="));
    Serial.println(g_temp_hum_sensor.config().stale_timeout_ms);

    Serial.print(F("light_driver: sensor="));
    Serial.print(node_env::LightSensor::sensorKindToString(kCompileTimeLightKind));
    Serial.print(F(" sim_estimate="));
    Serial.print(node_profile::kLightIsSimulation ? F("true") : F("false"));
#if defined(NODE_ENV_SIM)
    Serial.print(F(" adc_gpio="));
    Serial.print(node_profile::kLdrAdcPin);
#elif defined(NODE_ENV_REAL)
    Serial.print(F(" sda="));
    Serial.print(node_profile::kBh1750SdaPin);
    Serial.print(F(" scl="));
    Serial.print(node_profile::kBh1750SclPin);
    Serial.print(F(" i2c_addr=0x"));
    Serial.print(node_profile::kBh1750I2cAddress, HEX);
    Serial.print(F(" init_ok="));
    Serial.print(g_light_begin_ok ? F("true") : F("false"));
#endif
    Serial.print(F(" interval_ms="));
    Serial.print(g_light_sensor.config().sample_interval_ms);
    Serial.print(F(" stale_ms="));
    Serial.println(g_light_sensor.config().stale_timeout_ms);

    Serial.print(F("actuators: lamp_gpio="));
    Serial.print(g_actuator_sync.config().lamp_pin);
    Serial.print(F(" fan_gpio="));
    Serial.print(g_actuator_sync.config().fan_pin);
    Serial.print(F(" fan_role="));
    Serial.print(kFanOutputRole);
    Serial.print(F(" polarity="));
    Serial.print(node_env::NodeActuatorSync::polarityToString(kActuatorPolarity));
    Serial.print(F(" safe_boot_init="));
    Serial.print(act_out.initialized ? F("true") : F("false"));
    Serial.print(F(" lamp_level="));
    Serial.print(act_out.lamp_level_high ? F("HIGH") : F("LOW"));
    Serial.print(F(" fan_level="));
    Serial.println(act_out.fan_level_high ? F("HIGH") : F("LOW"));

    Serial.print(F("automation_safe_boot: mode="));
    Serial.print(node_env::AutomationController::modeToString(auto_state.mode));
    Serial.print(F(" lamp_on="));
    Serial.print(auto_state.lamp_on ? F("true") : F("false"));
    Serial.print(F(" fan_on="));
    Serial.print(auto_state.fan_on ? F("true") : F("false"));
    Serial.print(F(" sensor_health="));
    Serial.println(node_env::AutomationController::healthToString(auto_state.sensor_health));

    Serial.print(F("command_guard: dedup_cap="));
    Serial.print(static_cast<unsigned long>(g_command_guard.dedupCapacity()));
    Serial.print(F(" dedup_storage=ram_volatile_lost_on_reboot utc_format="));
    Serial.println(node_env::UtcParser::expectedFormat());

    const node_env::WiFiStatusSnapshot& wifi_snap = g_wifi_manager.snapshot();
    Serial.print(F("wifi_sta: config_source="));
    Serial.print(node_profile::kWifiConfigSource);
    Serial.print(F(" configured="));
    Serial.print(wifi_snap.configured ? F("true") : F("false"));
    Serial.print(F(" state="));
    Serial.print(node_env::WiFiConnectionManager::stateToString(wifi_snap.state));
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
    Serial.println(F("status=baseline_ready loop_mode=non_blocking_scheduler"));
    Serial.println(F("========================================"));
}

void logDhtStatus(uint32_t now_ms, const node_env::TempHumidityReading& reading) {
    const node_env::AutomationState& auto_state = g_automation.state();

    Serial.print(F("[sensor] type="));
    Serial.print(node_env::TempHumiditySensor::sensorKindToString(reading.sensor_kind));
    Serial.print(F(" sim="));
    Serial.print(node_profile::kDhtIsSimulation ? F("true") : F("false"));
    Serial.print(F(" gpio="));
    Serial.print(node_profile::kDhtDataPin);
    Serial.print(F(" status="));
    Serial.print(node_env::TempHumiditySensor::statusToString(reading.status));

    if (reading.valid) {
        Serial.print(F(" temp_c="));
        Serial.print(reading.temperature_c, 1);
        Serial.print(F(" humidity_pct="));
        Serial.print(reading.humidity_pct, 1);
    } else {
        Serial.print(F(" error="));
        Serial.print(node_env::TempHumiditySensor::errorToString(reading.error));
        Serial.print(F(" failures="));
        Serial.print(static_cast<unsigned long>(reading.consecutive_failures));
        Serial.print(F(" temp_c=NA humidity_pct=NA"));
    }

    Serial.print(F(" fan="));
    Serial.print(auto_state.fan_on ? 1 : 0);
    Serial.print(F(" health="));
    Serial.print(node_env::AutomationController::healthToString(auto_state.sensor_health));
    Serial.print(F(" uptime_ms="));
    Serial.println(now_ms);
}

void logLightStatus(uint32_t now_ms, const node_env::LightReading& reading) {
    const node_env::AutomationState& auto_state = g_automation.state();

    Serial.print(F("[light] type="));
    Serial.print(node_env::LightSensor::sensorKindToString(reading.sensor_kind));
    Serial.print(F(" sim_estimate="));
    Serial.print(reading.is_simulated_estimate ? F("true") : F("false"));
    Serial.print(F(" status="));
    Serial.print(node_env::LightSensor::statusToString(reading.status));

    if (reading.valid) {
        Serial.print(F(" lux="));
        Serial.print(reading.lux, 1);
    } else {
        Serial.print(F(" error="));
        Serial.print(node_env::LightSensor::errorToString(reading.error));
        Serial.print(F(" failures="));
        Serial.print(static_cast<unsigned long>(reading.consecutive_failures));
        Serial.print(F(" lux=NA"));
    }

    Serial.print(F(" lamp="));
    Serial.print(auto_state.lamp_on ? 1 : 0);
    Serial.print(F(" health="));
    Serial.print(node_env::AutomationController::healthToString(auto_state.sensor_health));
    Serial.print(F(" uptime_ms="));
    Serial.println(now_ms);
}

void logActuatorTransition(uint32_t now_ms, const node_env::ActuatorSyncOutcome& outcome) {
    const node_env::AutomationState& auto_state = g_automation.state();

    Serial.print(F("[actuator] mode="));
    Serial.print(node_env::AutomationController::modeToString(auto_state.mode));
    Serial.print(F(" lamp_gpio="));
    Serial.print(g_actuator_sync.config().lamp_pin);
    Serial.print(F(" lamp="));
    Serial.print(outcome.outputs.lamp_on ? 1 : 0);
    Serial.print(F(" fan_gpio="));
    Serial.print(g_actuator_sync.config().fan_pin);
    Serial.print(F(" fan="));
    Serial.print(outcome.outputs.fan_on ? 1 : 0);
    Serial.print(F(" health="));
    Serial.print(node_env::AutomationController::healthToString(auto_state.sensor_health));
    Serial.print(F(" uptime_ms="));
    Serial.println(now_ms);
}

void logWiFiStatus(uint32_t now_ms, const node_env::WiFiStatusSnapshot& snap) {
    Serial.print(F("[wifi] state="));
    Serial.print(node_env::WiFiConnectionManager::stateToString(snap.state));
    Serial.print(F(" err="));
    Serial.print(node_env::WiFiConnectionManager::errorToString(snap.last_error));
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
    const node_env::AutomationState& auto_state = g_automation.state();
    const node_env::WiFiStatusSnapshot& wifi_snap = g_wifi_manager.snapshot();

    ++g_heartbeat_seq;
    Serial.print(F("[heartbeat] node="));
    Serial.print(node_profile::kNodeId);
    Serial.print(F(" env="));
    Serial.print(node_profile::kBuildEnvName);
    Serial.print(F(" mode="));
    Serial.print(node_env::AutomationController::modeToString(auto_state.mode));
    Serial.print(F(" lamp="));
    Serial.print(auto_state.lamp_on ? 1 : 0);
    Serial.print(F(" fan="));
    Serial.print(auto_state.fan_on ? 1 : 0);
    Serial.print(F(" health="));
    Serial.print(node_env::AutomationController::healthToString(auto_state.sensor_health));
    Serial.print(F(" wifi_state="));
    Serial.print(node_env::WiFiConnectionManager::stateToString(wifi_snap.state));
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
    g_actuator_sync.beginSafeOff(g_gpio_sink);
    Serial.begin(node_profile::kSerialBaudRate);
    g_dht_driver.begin();
    g_light_begin_ok = g_light_driver.begin();
    g_wifi_manager.begin(g_wifi_adapter);
    logBootBanner();
    g_last_heartbeat_ms = millis();
}

void loop() {
    const uint32_t now_ms = millis();

    const node_env::WiFiStepOutcome wifi_outcome = g_wifi_manager.tick(now_ms, g_wifi_adapter);
    if (wifi_outcome.state_changed) {
        g_wifi_log_pending = true;
    }
    if (wifi_outcome.current_state != node_env::WiFiState::UNCONFIGURED &&
        g_wifi_log_pending &&
        node_env::WiFiConnectionManager::shouldEmitSerialLog(
            g_has_logged_wifi, now_ms, g_last_wifi_log_ms, true,
            node_profile::kWifiLogIntervalMs)) {
        logWiFiStatus(now_ms, wifi_outcome.snapshot);
        g_has_logged_wifi = true;
        g_last_wifi_log_ms = now_ms;
        g_wifi_log_pending = false;
    }

    const node_env::DhtPollOutcome dht_outcome = g_temp_hum_sensor.poll(now_ms, g_dht_driver);
    const node_env::LightPollOutcome light_outcome = g_light_sensor.poll(now_ms, g_light_driver);

    if (dht_outcome.attempted || dht_outcome.status_changed ||
        light_outcome.attempted || light_outcome.status_changed) {
        node_env::SensorReading auto_reading{};
        auto_reading.temperature_c = dht_outcome.reading.temperature_c;
        auto_reading.temperature_valid = dht_outcome.reading.valid;
        auto_reading.light_lux = light_outcome.reading.lux;
        auto_reading.light_lux_valid = light_outcome.reading.valid;
        g_automation.evaluateSensors(auto_reading);

        if ((dht_outcome.attempted || dht_outcome.status_changed) &&
            node_env::TempHumiditySensor::shouldEmitSerialLog(
                g_has_logged_dht, now_ms, g_last_dht_log_ms, dht_outcome.status_changed,
                node_profile::kSensorLogIntervalMs, node_profile::kDhtSampleIntervalMs)) {
            logDhtStatus(now_ms, dht_outcome.reading);
            g_has_logged_dht = true;
            g_last_dht_log_ms = now_ms;
        }

        if ((light_outcome.attempted || light_outcome.status_changed) &&
            node_env::TempHumiditySensor::shouldEmitSerialLog(
                g_has_logged_light, now_ms, g_last_light_log_ms, light_outcome.status_changed,
                node_profile::kSensorLogIntervalMs, node_profile::kLightSampleIntervalMs)) {
            logLightStatus(now_ms, light_outcome.reading);
            g_has_logged_light = true;
            g_last_light_log_ms = now_ms;
        }
    }

    const node_env::ActuatorSyncOutcome act_outcome =
        g_actuator_sync.sync(g_automation.state(), g_gpio_sink);
    if (act_outcome.changed) {
        logActuatorTransition(now_ms, act_outcome);
    }

    // Unsigned delta preserves correctness across the 32-bit millis() rollover.
    if (now_ms - g_last_heartbeat_ms >= node_profile::kHeartbeatIntervalMs) {
        g_last_heartbeat_ms = now_ms;
        logHeartbeat(now_ms);
    }
}
