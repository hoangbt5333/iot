#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "light_sensor.h"
#include "node_actuators.h"
#include "node_automation.h"
#include "temp_humidity_sensor.h"
#include "wifi_connection_manager.h"

using node_env::ActuatorPinConfig;
using node_env::ActuatorPolarity;
using node_env::AutomationController;
using node_env::DeviceMode;
using node_env::DhtSensorKind;
using node_env::IDhtRawReader;
using node_env::IGpioOutputSink;
using node_env::ILightRawReader;
using node_env::IWiFiAdapter;
using node_env::LightRawStatus;
using node_env::LightSensor;
using node_env::LightSensorConfig;
using node_env::LightSensorKind;
using node_env::NodeActuatorSync;
using node_env::SensorReading;
using node_env::TempHumidityConfig;
using node_env::TempHumiditySensor;
using node_env::WiFiAdapterStatus;
using node_env::WiFiConnectionManager;
using node_env::WiFiError;
using node_env::WiFiLinkStatus;
using node_env::WiFiState;
using node_env::WiFiStationConfig;
using node_env::WiFiStepOutcome;

namespace {

class FakeWiFiAdapter final : public IWiFiAdapter {
public:
    bool next_configure_ok = true;
    bool next_begin_connect_ok = true;
    WiFiAdapterStatus next_status{};

    uint32_t configure_calls = 0UL;
    uint32_t begin_connect_calls = 0UL;
    uint32_t disconnect_calls = 0UL;
    uint32_t poll_calls = 0UL;
    std::string last_ssid;
    std::string last_password;
    int32_t last_channel = -1;

    bool configureStation() override {
        ++configure_calls;
        return next_configure_ok;
    }

    bool beginConnect(const char* ssid, const char* password, int32_t channel) override {
        ++begin_connect_calls;
        last_ssid = (ssid != nullptr) ? ssid : "";
        last_password = (password != nullptr) ? password : "";
        last_channel = channel;
        return next_begin_connect_ok;
    }

    void disconnectAttempt() override { ++disconnect_calls; }

    WiFiAdapterStatus pollStatus() override {
        ++poll_calls;
        return next_status;
    }
};

class StubDhtReader final : public IDhtRawReader {
public:
    bool next_io_ok = true;
    float next_temp_c = 31.0f;
    float next_humidity_pct = 62.0f;

    bool readRawSample(float* out_temp_c, float* out_humidity_pct) override {
        if (out_temp_c != nullptr) {
            *out_temp_c = next_temp_c;
        }
        if (out_humidity_pct != nullptr) {
            *out_humidity_pct = next_humidity_pct;
        }
        return next_io_ok;
    }
};

class StubLightReader final : public ILightRawReader {
public:
    LightRawStatus next_status = LightRawStatus::OK;
    float next_lux = 45.0f;

    LightRawStatus readRawLux(float* out_lux) override {
        if (out_lux != nullptr) {
            *out_lux = next_lux;
        }
        return next_status;
    }
};

class FakeGpioSink final : public IGpioOutputSink {
public:
    bool pin26_high = false;
    bool pin25_high = false;

    void configureSafeOutput(uint8_t pin, bool initial_high) override {
        writeOutput(pin, initial_high);
    }

    void writeOutput(uint8_t pin, bool level_high) override {
        if (pin == 26U) {
            pin26_high = level_high;
        } else if (pin == 25U) {
            pin25_high = level_high;
        }
    }
};

WiFiStationConfig makeSimConfig() {
    WiFiStationConfig cfg;
    cfg.enabled = true;
    cfg.ssid = "Wokwi-GUEST";
    cfg.password = "";
    cfg.channel = 6;
    cfg.connect_timeout_ms = 2000UL;
    cfg.backoff_initial_ms = 1000UL;
    cfg.backoff_max_ms = 15000UL;
    return cfg;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_sim_config_uses_wokwi_guest_channel6_and_connects(void) {
    FakeWiFiAdapter adapter;
    WiFiConnectionManager manager(makeSimConfig());

    TEST_ASSERT_TRUE(manager.isConfigured());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::DISCONNECTED),
                            static_cast<uint8_t>(manager.state()));

    manager.begin(adapter);
    TEST_ASSERT_EQUAL_UINT32(1UL, adapter.configure_calls);

    const WiFiStepOutcome step1 = manager.tick(100UL, adapter);
    TEST_ASSERT_TRUE(step1.state_changed);
    TEST_ASSERT_TRUE(step1.connect_attempted);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                            static_cast<uint8_t>(step1.current_state));
    TEST_ASSERT_EQUAL_UINT32(1UL, adapter.begin_connect_calls);
    TEST_ASSERT_EQUAL_STRING("Wokwi-GUEST", adapter.last_ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("", adapter.last_password.c_str());
    TEST_ASSERT_EQUAL_INT32(6, adapter.last_channel);

    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTED, true, -48};
    const WiFiStepOutcome step2 = manager.tick(250UL, adapter);
    TEST_ASSERT_TRUE(step2.state_changed);
    TEST_ASSERT_FALSE(step2.connect_attempted);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTED),
                            static_cast<uint8_t>(step2.current_state));
    TEST_ASSERT_TRUE(manager.isConnected());
    TEST_ASSERT_TRUE(step2.snapshot.has_valid_ip);
    TEST_ASSERT_EQUAL_INT8(-48, step2.snapshot.rssi_dbm);
    TEST_ASSERT_EQUAL_UINT32(1UL, step2.snapshot.successful_connects);
}

void test_real_unconfigured_stays_dormant_and_never_calls_adapter(void) {
    FakeWiFiAdapter adapter;
    WiFiStationConfig real_cfg;
    real_cfg.enabled = false;
    real_cfg.ssid = "";
    real_cfg.password = "";
    real_cfg.channel = 0;

    WiFiConnectionManager manager(real_cfg);
    TEST_ASSERT_FALSE(manager.isConfigured());
    TEST_ASSERT_FALSE(manager.isConnected());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::UNCONFIGURED),
                            static_cast<uint8_t>(manager.state()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::UNCONFIGURED),
                            static_cast<uint8_t>(manager.lastError()));

    manager.begin(adapter);
    for (uint32_t t = 0UL; t <= 30000UL; t += 1000UL) {
        const WiFiStepOutcome step = manager.tick(t, adapter);
        TEST_ASSERT_FALSE(step.state_changed);
        TEST_ASSERT_FALSE(step.connect_attempted);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::UNCONFIGURED),
                                static_cast<uint8_t>(step.current_state));
    }

    TEST_ASSERT_EQUAL_UINT32(0UL, adapter.configure_calls);
    TEST_ASSERT_EQUAL_UINT32(0UL, adapter.begin_connect_calls);
    TEST_ASSERT_EQUAL_UINT32(0UL, adapter.poll_calls);
}

void test_invalid_configs_enter_config_error_and_never_connect(void) {
    FakeWiFiAdapter adapter;

    WiFiStationConfig missing_ssid = makeSimConfig();
    missing_ssid.ssid = "";
    WiFiConnectionManager mgr_missing(missing_ssid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONFIG_ERROR),
                            static_cast<uint8_t>(mgr_missing.state()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::INVALID_CONFIG),
                            static_cast<uint8_t>(mgr_missing.lastError()));

    const std::string long_ssid(33U, 'A');
    WiFiStationConfig too_long_ssid = makeSimConfig();
    too_long_ssid.ssid = long_ssid.c_str();
    WiFiConnectionManager mgr_ssid(too_long_ssid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONFIG_ERROR),
                            static_cast<uint8_t>(mgr_ssid.state()));

    const std::string long_pass(65U, 'P');
    WiFiStationConfig too_long_pass = makeSimConfig();
    too_long_pass.password = long_pass.c_str();
    WiFiConnectionManager mgr_pass(too_long_pass);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONFIG_ERROR),
                            static_cast<uint8_t>(mgr_pass.state()));

    WiFiStationConfig bad_channel = makeSimConfig();
    bad_channel.channel = 15;
    WiFiConnectionManager mgr_chan(bad_channel);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONFIG_ERROR),
                            static_cast<uint8_t>(mgr_chan.state()));

    mgr_missing.begin(adapter);
    mgr_missing.tick(100UL, adapter);
    mgr_ssid.tick(100UL, adapter);
    mgr_pass.tick(100UL, adapter);
    mgr_chan.tick(100UL, adapter);

    TEST_ASSERT_EQUAL_UINT32(0UL, adapter.configure_calls);
    TEST_ASSERT_EQUAL_UINT32(0UL, adapter.begin_connect_calls);
}

void test_avoids_duplicate_begin_while_connecting_or_connected(void) {
    FakeWiFiAdapter adapter;
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::IDLE, false, 0};
    WiFiConnectionManager manager(makeSimConfig());

    manager.tick(0UL, adapter);
    TEST_ASSERT_EQUAL_UINT32(1UL, adapter.begin_connect_calls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                            static_cast<uint8_t>(manager.state()));

    for (uint32_t t = 50UL; t < 2000UL; t += 100UL) {
        const WiFiStepOutcome step = manager.tick(t, adapter);
        TEST_ASSERT_FALSE(step.connect_attempted);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                                static_cast<uint8_t>(step.current_state));
    }
    TEST_ASSERT_EQUAL_UINT32(1UL, adapter.begin_connect_calls);

    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTED, true, -55};
    manager.tick(1950UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTED),
                            static_cast<uint8_t>(manager.state()));

    for (uint32_t t = 2000UL; t <= 10000UL; t += 500UL) {
        const WiFiStepOutcome step = manager.tick(t, adapter);
        TEST_ASSERT_FALSE(step.connect_attempted);
        TEST_ASSERT_FALSE(step.state_changed);
        TEST_ASSERT_TRUE(manager.isConnected());
    }
    TEST_ASSERT_EQUAL_UINT32(1UL, adapter.begin_connect_calls);
}

void test_requires_wl_connected_and_valid_ip_before_connected(void) {
    FakeWiFiAdapter adapter;
    WiFiConnectionManager manager(makeSimConfig());

    manager.tick(0UL, adapter);
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTED, false, -60};

    const WiFiStepOutcome waiting_ip = manager.tick(1000UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                            static_cast<uint8_t>(waiting_ip.current_state));
    TEST_ASSERT_FALSE(manager.isConnected());

    const WiFiStepOutcome timed_out = manager.tick(2000UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(timed_out.current_state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::CONNECTED_WITHOUT_IP),
                            static_cast<uint8_t>(timed_out.snapshot.last_error));
    TEST_ASSERT_EQUAL_UINT32(1UL, adapter.disconnect_calls);
}

void test_timeout_applies_exponential_backoff_with_upper_bound(void) {
    FakeWiFiAdapter adapter;
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::DISCONNECTED, false, 0};

    WiFiStationConfig cfg = makeSimConfig();
    cfg.connect_timeout_ms = 500UL;
    cfg.backoff_initial_ms = 1000UL;
    cfg.backoff_max_ms = 15000UL;
    WiFiConnectionManager manager(cfg);

    uint32_t now = 0UL;
    const uint32_t expected_backoffs[] = {1000UL, 2000UL, 4000UL, 8000UL, 15000UL, 15000UL};

    for (uint32_t i = 0UL; i < 6UL; ++i) {
        const WiFiStepOutcome start = manager.tick(now, adapter);
        TEST_ASSERT_TRUE(start.connect_attempted);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                                static_cast<uint8_t>(start.current_state));

        now += 500UL;
        const WiFiStepOutcome timeout = manager.tick(now, adapter);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                                static_cast<uint8_t>(timeout.current_state));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::CONNECT_TIMEOUT),
                                static_cast<uint8_t>(timeout.snapshot.last_error));
        TEST_ASSERT_EQUAL_UINT32(expected_backoffs[i], manager.currentBackoffMs());

        // Must stay in BACKOFF without tight-loop retries before backoff expires.
        const WiFiStepOutcome still_waiting =
            manager.tick(now + expected_backoffs[i] - 1UL, adapter);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                                static_cast<uint8_t>(still_waiting.current_state));
        TEST_ASSERT_FALSE(still_waiting.connect_attempted);

        now += expected_backoffs[i];
    }

    TEST_ASSERT_EQUAL_UINT32(6UL, adapter.begin_connect_calls);
    TEST_ASSERT_EQUAL_UINT32(6UL, adapter.disconnect_calls);
    TEST_ASSERT_EQUAL_UINT32(6UL, manager.snapshot().connect_failures);
}

void test_disconnect_after_connected_backs_off_and_resets_backoff_after_recovery(void) {
    FakeWiFiAdapter adapter;
    WiFiConnectionManager manager(makeSimConfig());

    // First failure escalates backoff to 1000 ms, then second attempt connects.
    manager.tick(0UL, adapter);
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::NO_SSID_AVAIL, false, 0};
    const WiFiStepOutcome fail1 = manager.tick(100UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(fail1.current_state));
    TEST_ASSERT_EQUAL_UINT32(1000UL, manager.currentBackoffMs());

    // Fail once more after backoff so backoff reaches 2000 ms before succeeding.
    manager.tick(1100UL, adapter);
    const WiFiStepOutcome fail2 = manager.tick(1200UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(fail2.current_state));
    TEST_ASSERT_EQUAL_UINT32(2000UL, manager.currentBackoffMs());

    // Recover at 3200 ms.
    manager.tick(3200UL, adapter);
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTED, true, -52};
    const WiFiStepOutcome connected = manager.tick(3400UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTED),
                            static_cast<uint8_t>(connected.current_state));
    TEST_ASSERT_EQUAL_UINT32(0UL, connected.snapshot.consecutive_failures);

    // Drop link after being connected: backoff must restart at initial 1000 ms.
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTION_LOST, false, 0};
    const WiFiStepOutcome dropped = manager.tick(5000UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(dropped.current_state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::LINK_LOST),
                            static_cast<uint8_t>(dropped.snapshot.last_error));
    TEST_ASSERT_EQUAL_UINT32(1UL, dropped.snapshot.disconnect_count);
    TEST_ASSERT_EQUAL_UINT32(1000UL, manager.currentBackoffMs());

    // Recover again after 1000 ms.
    manager.tick(6000UL, adapter);
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTED, true, -50};
    const WiFiStepOutcome recovered = manager.tick(6200UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTED),
                            static_cast<uint8_t>(recovered.current_state));
    TEST_ASSERT_EQUAL_UINT32(2UL, recovered.snapshot.successful_connects);
}

void test_millis_uint32_rollover_in_timeout_and_backoff(void) {
    FakeWiFiAdapter adapter;
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::IDLE, false, 0};

    WiFiStationConfig cfg = makeSimConfig();
    cfg.connect_timeout_ms = 500UL;
    cfg.backoff_initial_ms = 1000UL;
    WiFiConnectionManager manager(cfg);

    // Start attempt just before 32-bit millis() wraparound.
    manager.tick(0xFFFFFF00UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                            static_cast<uint8_t>(manager.state()));

    // 250 ms elapsed across wrap edge -> still CONNECTING.
    manager.tick(0xFFFFFFFAUL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                            static_cast<uint8_t>(manager.state()));

    // 512 ms elapsed (0x00000100 - 0xFFFFFF00 = 0x200 = 512 ms) -> times out to BACKOFF.
    const WiFiStepOutcome timed_out = manager.tick(0x00000100UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(timed_out.current_state));

    // Advance 999 ms from 0x00000100 -> still BACKOFF; at +1000 ms -> retries.
    manager.tick(0x000004E7UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(manager.state()));

    const WiFiStepOutcome retry = manager.tick(0x000004E8UL, adapter);
    TEST_ASSERT_TRUE(retry.connect_attempted);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                            static_cast<uint8_t>(retry.current_state));
}

void test_adapter_failures_and_recovery(void) {
    FakeWiFiAdapter adapter;
    WiFiConnectionManager manager(makeSimConfig());

    // 1. configureStation failure enters BACKOFF and retries when healthy.
    adapter.next_configure_ok = false;
    const WiFiStepOutcome cfg_fail = manager.tick(0UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(cfg_fail.current_state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::ADAPTER_INIT_FAILED),
                            static_cast<uint8_t>(cfg_fail.snapshot.last_error));
    TEST_ASSERT_EQUAL_UINT32(0UL, adapter.begin_connect_calls);

    // 2. configureStation recovers, beginConnect fails -> BACKOFF(BEGIN_CONNECT_FAILED).
    adapter.next_configure_ok = true;
    adapter.next_begin_connect_ok = false;
    const WiFiStepOutcome begin_fail = manager.tick(1000UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(begin_fail.current_state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::BEGIN_CONNECT_FAILED),
                            static_cast<uint8_t>(begin_fail.snapshot.last_error));
    TEST_ASSERT_EQUAL_UINT32(1UL, adapter.begin_connect_calls);

    // 3. Recover to CONNECTED, then simulate pollStatus adapter_ok=false -> re-configures cleanly.
    adapter.next_begin_connect_ok = true;
    manager.tick(3000UL, adapter);
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTED, true, -60};
    manager.tick(3100UL, adapter);
    TEST_ASSERT_TRUE(manager.isConnected());

    adapter.next_status = WiFiAdapterStatus{false, WiFiLinkStatus::HARDWARE_UNAVAILABLE, false, 0};
    const WiFiStepOutcome hw_drop = manager.tick(3200UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(hw_drop.current_state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::ADAPTER_INIT_FAILED),
                            static_cast<uint8_t>(hw_drop.snapshot.last_error));

    const uint32_t cfg_calls_before = adapter.configure_calls;
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::IDLE, false, 0};
    manager.tick(4200UL, adapter);
    TEST_ASSERT_EQUAL_UINT32(cfg_calls_before + 1UL, adapter.configure_calls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                            static_cast<uint8_t>(manager.state()));
}

void test_wifi_offline_or_fault_preserves_local_automation_and_actuators(void) {
    FakeWiFiAdapter wifi_adapter;
    wifi_adapter.next_configure_ok = false;  // Persistent Wi-Fi failure

    WiFiConnectionManager wifi_mgr(makeSimConfig());
    StubDhtReader dht_reader;
    StubLightReader light_reader;
    FakeGpioSink gpio_sink;

    TempHumiditySensor dht(TempHumidityConfig{DhtSensorKind::DHT11, 2000UL, 6000UL});
    LightSensor light(LightSensorConfig{LightSensorKind::BH1750, 1000UL, 5000UL, 150000.0f});
    AutomationController automation;
    NodeActuatorSync actuators(
        ActuatorPinConfig{26U, 25U, ActuatorPolarity::ACTIVE_HIGH, ActuatorPolarity::ACTIVE_HIGH});
    actuators.beginSafeOff(gpio_sink);

    // Tick Wi-Fi (into BACKOFF) alongside sensors, AutomationController, and GPIO25/26.
    wifi_mgr.tick(0UL, wifi_adapter);
    TEST_ASSERT_FALSE(wifi_mgr.isConnected());

    const auto dht_step = dht.poll(0UL, dht_reader);
    const auto light_step = light.poll(0UL, light_reader);
    SensorReading reading{};
    reading.temperature_c = dht_step.reading.temperature_c;
    reading.temperature_valid = dht_step.reading.valid;
    reading.light_lux = light_step.reading.lux;
    reading.light_lux_valid = light_step.reading.valid;
    automation.evaluateSensors(reading);
    actuators.sync(automation.state(), gpio_sink);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceMode::AUTO),
                            static_cast<uint8_t>(automation.state().mode));
    TEST_ASSERT_TRUE(automation.state().lamp_on);   // 45 lux < 100 lux ON threshold
    TEST_ASSERT_TRUE(automation.state().fan_on);    // 31 C >= 30 C ON threshold
    TEST_ASSERT_TRUE(gpio_sink.pin26_high);
    TEST_ASSERT_TRUE(gpio_sink.pin25_high);

    // Also verify UNCONFIGURED mode keeps local hysteresis working (turn both OFF).
    WiFiConnectionManager real_unconfigured(WiFiStationConfig{});
    real_unconfigured.tick(2000UL, wifi_adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::UNCONFIGURED),
                            static_cast<uint8_t>(real_unconfigured.state()));

    dht_reader.next_temp_c = 27.0f;
    light_reader.next_lux = 180.0f;
    const auto dht_step2 = dht.poll(2000UL, dht_reader);
    const auto light_step2 = light.poll(2000UL, light_reader);
    reading.temperature_c = dht_step2.reading.temperature_c;
    reading.temperature_valid = dht_step2.reading.valid;
    reading.light_lux = light_step2.reading.lux;
    reading.light_lux_valid = light_step2.reading.valid;
    automation.evaluateSensors(reading);
    actuators.sync(automation.state(), gpio_sink);

    TEST_ASSERT_FALSE(automation.state().lamp_on);
    TEST_ASSERT_FALSE(automation.state().fan_on);
    TEST_ASSERT_FALSE(gpio_sink.pin26_high);
    TEST_ASSERT_FALSE(gpio_sink.pin25_high);
}

void test_serial_log_rate_limiting_and_rollover(void) {
    TEST_ASSERT_TRUE(WiFiConnectionManager::shouldEmitSerialLog(false, 0UL, 0UL, false, 5000UL));
    TEST_ASSERT_FALSE(WiFiConnectionManager::shouldEmitSerialLog(true, 500UL, 0UL, true, 5000UL));
    TEST_ASSERT_TRUE(WiFiConnectionManager::shouldEmitSerialLog(true, 1000UL, 0UL, true, 5000UL));
    TEST_ASSERT_FALSE(
        WiFiConnectionManager::shouldEmitSerialLog(true, 4999UL, 0UL, false, 5000UL));
    TEST_ASSERT_TRUE(WiFiConnectionManager::shouldEmitSerialLog(true, 5000UL, 0UL, false, 5000UL));
    // Across uint32_t rollover: 0x00000100 - 0xFFFFFF00 = 512 ms (< 1000 ms) vs 0x00000300 = 1024 ms.
    TEST_ASSERT_FALSE(WiFiConnectionManager::shouldEmitSerialLog(
        true, 0x00000100UL, 0xFFFFFF00UL, true, 5000UL));
    TEST_ASSERT_TRUE(WiFiConnectionManager::shouldEmitSerialLog(
        true, 0x00000300UL, 0xFFFFFF00UL, true, 5000UL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sim_config_uses_wokwi_guest_channel6_and_connects);
    RUN_TEST(test_real_unconfigured_stays_dormant_and_never_calls_adapter);
    RUN_TEST(test_invalid_configs_enter_config_error_and_never_connect);
    RUN_TEST(test_avoids_duplicate_begin_while_connecting_or_connected);
    RUN_TEST(test_requires_wl_connected_and_valid_ip_before_connected);
    RUN_TEST(test_timeout_applies_exponential_backoff_with_upper_bound);
    RUN_TEST(test_disconnect_after_connected_backs_off_and_resets_backoff_after_recovery);
    RUN_TEST(test_millis_uint32_rollover_in_timeout_and_backoff);
    RUN_TEST(test_adapter_failures_and_recovery);
    RUN_TEST(test_wifi_offline_or_fault_preserves_local_automation_and_actuators);
    RUN_TEST(test_serial_log_rate_limiting_and_rollover);
    return UNITY_END();
}
