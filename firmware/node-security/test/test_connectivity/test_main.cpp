#include <unity.h>

#include <cstdint>
#include <string>

#include "node_security.h"
#include "security_inputs.h"
#include "security_outputs.h"
#include "security_servo.h"
#include "wifi_connection_manager.h"

using node_sec::AlarmOutputConfig;
using node_sec::AlarmOutputSync;
using node_sec::CommandAction;
using node_sec::CommandStatus;
using node_sec::CommandTarget;
using node_sec::IAlarmOutputSink;
using node_sec::ISecurityInputSource;
using node_sec::IServoOutputSink;
using node_sec::IWiFiAdapter;
using node_sec::NodeBCommand;
using node_sec::RawSecurityInputSample;
using node_sec::SecurityController;
using node_sec::SecurityInputSampler;
using node_sec::SecurityInputSamplerConfig;
using node_sec::SecurityMode;
using node_sec::SecuritySamplerStep;
using node_sec::ServoOutputConfig;
using node_sec::ServoOutputSync;
using node_sec::SwitchArbitrationMode;
using node_sec::WiFiAdapterStatus;
using node_sec::WiFiConnectionManager;
using node_sec::WiFiError;
using node_sec::WiFiLinkStatus;
using node_sec::WiFiState;
using node_sec::WiFiStationConfig;
using node_sec::WiFiStepOutcome;

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

class StubSecurityInputSource final : public ISecurityInputSource {
public:
    bool initialized = false;
    RawSecurityInputSample next_sample{true, false, false};

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
};

class FakeAlarmOutputSink final : public IAlarmOutputSink {
public:
    bool buzzer_high = false;
    bool led_high = false;
    uint32_t write_calls = 0UL;

    bool configureOutput(uint8_t pin, bool initial_high) override {
        return writeOutput(pin, initial_high);
    }

    bool writeOutput(uint8_t pin, bool level_high) override {
        ++write_calls;
        if (pin == 18U) {
            buzzer_high = level_high;
            return true;
        }
        if (pin == 26U) {
            led_high = level_high;
            return true;
        }
        return false;
    }
};

class FakeServoOutputSink final : public IServoOutputSink {
public:
    bool attached = false;
    uint32_t attach_calls = 0UL;
    uint32_t write_calls = 0UL;
    int16_t last_angle = 0;

    bool isAttached() const override { return attached; }

    bool attach(uint8_t, uint16_t, uint16_t, uint16_t) override {
        ++attach_calls;
        attached = true;
        return true;
    }

    bool writeAngle(int16_t angle_deg) override {
        ++write_calls;
        last_angle = angle_deg;
        return true;
    }

    void detach() override { attached = false; }
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

    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTED, true, -46};
    const WiFiStepOutcome step2 = manager.tick(250UL, adapter);
    TEST_ASSERT_TRUE(step2.state_changed);
    TEST_ASSERT_FALSE(step2.connect_attempted);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTED),
                            static_cast<uint8_t>(step2.current_state));
    TEST_ASSERT_TRUE(manager.isConnected());
    TEST_ASSERT_TRUE(step2.snapshot.has_valid_ip);
    TEST_ASSERT_EQUAL_INT8(-46, step2.snapshot.rssi_dbm);
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

    const std::string long_ssid(33U, 'S');
    WiFiStationConfig too_long_ssid = makeSimConfig();
    too_long_ssid.ssid = long_ssid.c_str();
    WiFiConnectionManager mgr_ssid(too_long_ssid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONFIG_ERROR),
                            static_cast<uint8_t>(mgr_ssid.state()));

    const std::string long_pass(65U, 'K');
    WiFiStationConfig too_long_pass = makeSimConfig();
    too_long_pass.password = long_pass.c_str();
    WiFiConnectionManager mgr_pass(too_long_pass);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONFIG_ERROR),
                            static_cast<uint8_t>(mgr_pass.state()));

    WiFiStationConfig bad_channel = makeSimConfig();
    bad_channel.channel = -1;
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

    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTED, true, -54};
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

    manager.tick(0UL, adapter);
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::NO_SSID_AVAIL, false, 0};
    const WiFiStepOutcome fail1 = manager.tick(100UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(fail1.current_state));
    TEST_ASSERT_EQUAL_UINT32(1000UL, manager.currentBackoffMs());

    manager.tick(1100UL, adapter);
    const WiFiStepOutcome fail2 = manager.tick(1200UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(fail2.current_state));
    TEST_ASSERT_EQUAL_UINT32(2000UL, manager.currentBackoffMs());

    manager.tick(3200UL, adapter);
    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTED, true, -52};
    const WiFiStepOutcome connected = manager.tick(3400UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTED),
                            static_cast<uint8_t>(connected.current_state));
    TEST_ASSERT_EQUAL_UINT32(0UL, connected.snapshot.consecutive_failures);

    adapter.next_status = WiFiAdapterStatus{true, WiFiLinkStatus::CONNECTION_LOST, false, 0};
    const WiFiStepOutcome dropped = manager.tick(5000UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(dropped.current_state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::LINK_LOST),
                            static_cast<uint8_t>(dropped.snapshot.last_error));
    TEST_ASSERT_EQUAL_UINT32(1UL, dropped.snapshot.disconnect_count);
    TEST_ASSERT_EQUAL_UINT32(1000UL, manager.currentBackoffMs());

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

    manager.tick(0xFFFFFF00UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                            static_cast<uint8_t>(manager.state()));

    manager.tick(0xFFFFFFFAUL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::CONNECTING),
                            static_cast<uint8_t>(manager.state()));

    const WiFiStepOutcome timed_out = manager.tick(0x00000100UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(timed_out.current_state));

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

    adapter.next_configure_ok = false;
    const WiFiStepOutcome cfg_fail = manager.tick(0UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(cfg_fail.current_state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::ADAPTER_INIT_FAILED),
                            static_cast<uint8_t>(cfg_fail.snapshot.last_error));
    TEST_ASSERT_EQUAL_UINT32(0UL, adapter.begin_connect_calls);

    adapter.next_configure_ok = true;
    adapter.next_begin_connect_ok = false;
    const WiFiStepOutcome begin_fail = manager.tick(1000UL, adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(begin_fail.current_state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiError::BEGIN_CONNECT_FAILED),
                            static_cast<uint8_t>(begin_fail.snapshot.last_error));
    TEST_ASSERT_EQUAL_UINT32(1UL, adapter.begin_connect_calls);

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

void test_serial_log_rate_limiting_and_rollover(void) {
    TEST_ASSERT_TRUE(WiFiConnectionManager::shouldEmitSerialLog(false, 0UL, 0UL, false, 5000UL));
    TEST_ASSERT_FALSE(WiFiConnectionManager::shouldEmitSerialLog(true, 500UL, 0UL, true, 5000UL));
    TEST_ASSERT_TRUE(WiFiConnectionManager::shouldEmitSerialLog(true, 1000UL, 0UL, true, 5000UL));
    TEST_ASSERT_FALSE(
        WiFiConnectionManager::shouldEmitSerialLog(true, 4999UL, 0UL, false, 5000UL));
    TEST_ASSERT_TRUE(WiFiConnectionManager::shouldEmitSerialLog(true, 5000UL, 0UL, false, 5000UL));
    TEST_ASSERT_FALSE(WiFiConnectionManager::shouldEmitSerialLog(
        true, 0x00000100UL, 0xFFFFFF00UL, true, 5000UL));
    TEST_ASSERT_TRUE(WiFiConnectionManager::shouldEmitSerialLog(
        true, 0x00000300UL, 0xFFFFFF00UL, true, 5000UL));
}

void test_wifi_offline_or_fault_preserves_node_b_security_inputs_outputs_and_servo(void) {
    FakeWiFiAdapter wifi_adapter;
    wifi_adapter.next_configure_ok = false;  // Persistent Wi-Fi failure

    WiFiConnectionManager wifi_sim_fault(makeSimConfig());
    WiFiConnectionManager wifi_real_unconfigured(WiFiStationConfig{});

    SecurityController security;
    StubSecurityInputSource input_source;
    SecurityInputSampler sampler(SecurityInputSamplerConfig{});
    FakeAlarmOutputSink alarm_sink;
    AlarmOutputSync alarm_outputs(AlarmOutputConfig{});
    FakeServoOutputSink servo_sink;
    ServoOutputSync servo_outputs(ServoOutputConfig{});

    TEST_ASSERT_TRUE(alarm_outputs.begin(alarm_sink));
    TEST_ASSERT_TRUE(servo_outputs.begin());
    TEST_ASSERT_TRUE(input_source.begin());

    // Safe boot invariants preserved regardless of Wi-Fi state.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::HOME),
                            static_cast<uint8_t>(security.state().mode));
    TEST_ASSERT_FALSE(security.config().pir_triggers_alarm_in_away);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(SwitchArbitrationMode::DISABLED_PENDING_APPROVAL),
        static_cast<uint8_t>(security.config().switch_arbitration));
    TEST_ASSERT_FALSE(alarm_sink.buzzer_high);
    TEST_ASSERT_FALSE(alarm_sink.led_high);
    TEST_ASSERT_FALSE(servo_sink.attached);

    // Tick both failing SIM Wi-Fi and UNCONFIGURED REAL Wi-Fi alongside Node B loop.
    wifi_sim_fault.tick(0UL, wifi_adapter);
    wifi_real_unconfigured.tick(0UL, wifi_adapter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::BACKOFF),
                            static_cast<uint8_t>(wifi_sim_fault.state()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WiFiState::UNCONFIGURED),
                            static_cast<uint8_t>(wifi_real_unconfigured.state()));

    // With door closed & PIR low, Wi-Fi failures must never synthesize events or change mode.
    const SecuritySamplerStep idle_step = sampler.poll(input_source, security, 0UL);
    alarm_outputs.sync(alarm_sink, security);
    servo_outputs.sync(servo_sink, security);
    TEST_ASSERT_TRUE(idle_step.polled);
    TEST_ASSERT_FALSE(idle_step.door_outcome.door_changed);
    TEST_ASSERT_FALSE(idle_step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_FALSE(idle_step.any_alarm_triggered);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::HOME),
                            static_cast<uint8_t>(security.state().mode));
    TEST_ASSERT_FALSE(alarm_sink.buzzer_high);
    TEST_ASSERT_FALSE(alarm_sink.led_high);
    TEST_ASSERT_EQUAL_UINT32(0UL, servo_sink.attach_calls);

    // In HOME mode with Wi-Fi offline, PIR motion + debounced door open update state without alarm or servo.
    input_source.next_sample = RawSecurityInputSample{true, true, true};
    sampler.poll(input_source, security, 10UL);
    const SecuritySamplerStep home_open_step = sampler.poll(input_source, security, 60UL);
    alarm_outputs.sync(alarm_sink, security);
    servo_outputs.sync(servo_sink, security);
    TEST_ASSERT_TRUE(home_open_step.door_outcome.door_changed);
    TEST_ASSERT_TRUE(home_open_step.state.door_open);
    TEST_ASSERT_TRUE(home_open_step.state.motion_detected);
    TEST_ASSERT_FALSE(home_open_step.state.alarm_on);
    TEST_ASSERT_FALSE(alarm_sink.buzzer_high);
    TEST_ASSERT_FALSE(alarm_sink.led_high);
    TEST_ASSERT_EQUAL_UINT32(0UL, servo_sink.attach_calls);

    // Close door, switch to AWAY via validated command, and verify edge alarm still works offline.
    input_source.next_sample = RawSecurityInputSample{true, false, false};
    sampler.poll(input_source, security, 70UL);
    sampler.poll(input_source, security, 120UL);

    NodeBCommand away_cmd{};
    away_cmd.target = CommandTarget::SECURITY_MODE;
    away_cmd.action = CommandAction::SET;
    away_cmd.has_mode_value = true;
    away_cmd.mode_value = SecurityMode::AWAY;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandStatus::APPLIED),
                            static_cast<uint8_t>(security.applyCommand(away_cmd).status));

    // PIR alone in AWAY must NOT trigger alarm (policy preserved), servo stays unattached.
    input_source.next_sample = RawSecurityInputSample{true, true, false};
    const SecuritySamplerStep away_pir_step = sampler.poll(input_source, security, 130UL);
    alarm_outputs.sync(alarm_sink, security);
    servo_outputs.sync(servo_sink, security);
    TEST_ASSERT_TRUE(away_pir_step.motion_outcome.motion_rising_edge);
    TEST_ASSERT_FALSE(security.state().alarm_on);
    TEST_ASSERT_FALSE(alarm_sink.buzzer_high);
    TEST_ASSERT_FALSE(alarm_sink.led_high);
    TEST_ASSERT_EQUAL_UINT32(0UL, servo_sink.attach_calls);

    // Door opens in AWAY while Wi-Fi remains in BACKOFF/UNCONFIGURED -> local edge alarm activates!
    input_source.next_sample = RawSecurityInputSample{true, false, true};
    sampler.poll(input_source, security, 140UL);
    const SecuritySamplerStep away_door_step = sampler.poll(input_source, security, 190UL);
    alarm_outputs.sync(alarm_sink, security);
    servo_outputs.sync(servo_sink, security);
    TEST_ASSERT_TRUE(away_door_step.any_alarm_triggered);
    TEST_ASSERT_TRUE(security.state().alarm_on);
    TEST_ASSERT_TRUE(alarm_sink.buzzer_high);
    TEST_ASSERT_TRUE(alarm_sink.led_high);
    TEST_ASSERT_EQUAL_UINT32(0UL, servo_sink.attach_calls);
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
    RUN_TEST(test_serial_log_rate_limiting_and_rollover);
    RUN_TEST(test_wifi_offline_or_fault_preserves_node_b_security_inputs_outputs_and_servo);
    return UNITY_END();
}
