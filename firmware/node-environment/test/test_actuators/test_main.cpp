#include <unity.h>

#include <cstdint>
#include <limits>

#include "light_sensor.h"
#include "node_actuators.h"
#include "node_automation.h"
#include "temp_humidity_sensor.h"

using node_env::ActuatorOutputs;
using node_env::ActuatorPinConfig;
using node_env::ActuatorPolarity;
using node_env::ActuatorSyncOutcome;
using node_env::AutomationController;
using node_env::AutomationState;
using node_env::DhtPollOutcome;
using node_env::DhtSensorKind;
using node_env::IDhtRawReader;
using node_env::IGpioOutputSink;
using node_env::ILightRawReader;
using node_env::LightPollOutcome;
using node_env::LightRawStatus;
using node_env::LightSensor;
using node_env::LightSensorConfig;
using node_env::LightSensorKind;
using node_env::NodeActuatorSync;
using node_env::SensorHealth;
using node_env::SensorReading;
using node_env::TempHumidityConfig;
using node_env::TempHumiditySensor;

namespace {

class FakeGpioSink final : public IGpioOutputSink {
public:
    uint32_t configure_calls = 0UL;
    uint32_t write_calls = 0UL;
    uint32_t pin26_writes = 0UL;
    uint32_t pin25_writes = 0UL;
    bool pin26_configured = false;
    bool pin25_configured = false;
    bool pin26_level_high = true;  // Start poisoned true so beginSafeOff must actively set LOW
    bool pin25_level_high = true;

    void configureSafeOutput(uint8_t pin, bool initial_high) override {
        ++configure_calls;
        if (pin == 26U) {
            pin26_configured = true;
            pin26_level_high = initial_high;
        } else if (pin == 25U) {
            pin25_configured = true;
            pin25_level_high = initial_high;
        }
    }

    void writeOutput(uint8_t pin, bool level_high) override {
        ++write_calls;
        if (pin == 26U) {
            ++pin26_writes;
            pin26_level_high = level_high;
        } else if (pin == 25U) {
            ++pin25_writes;
            pin25_level_high = level_high;
        }
    }
};

class StubDhtReader final : public IDhtRawReader {
public:
    bool next_io_ok = true;
    float next_temp_c = 25.0f;
    float next_humidity_pct = 55.0f;

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
    float next_lux = 125.0f;

    LightRawStatus readRawLux(float* out_lux) override {
        if (out_lux != nullptr) {
            *out_lux = next_lux;
        }
        return next_status;
    }
};

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_boot_safe_off_and_no_redundant_writes_on_unchanged_state(void) {
    FakeGpioSink sink;
    AutomationController automation;
    NodeActuatorSync sync;

    TEST_ASSERT_FALSE(sync.isInitialized());
    TEST_ASSERT_EQUAL_UINT8(26U, sync.config().lamp_pin);
    TEST_ASSERT_EQUAL_UINT8(25U, sync.config().fan_pin);

    sync.beginSafeOff(sink);
    TEST_ASSERT_TRUE(sync.isInitialized());
    TEST_ASSERT_EQUAL_UINT32(2UL, sink.configure_calls);
    TEST_ASSERT_TRUE(sink.pin26_configured);
    TEST_ASSERT_TRUE(sink.pin25_configured);
    TEST_ASSERT_FALSE(sink.pin26_level_high);  // GPIO26 LOW (OFF)
    TEST_ASSERT_FALSE(sink.pin25_level_high);  // GPIO25 LOW (OFF)
    TEST_ASSERT_EQUAL_UINT32(0UL, sink.write_calls);

    // Syncing with initial safe AutomationState (both OFF) must perform 0 GPIO writes.
    for (int i = 0; i < 10; ++i) {
        ActuatorSyncOutcome out = sync.sync(automation.state(), sink);
        TEST_ASSERT_FALSE(out.changed);
        TEST_ASSERT_FALSE(out.lamp_written);
        TEST_ASSERT_FALSE(out.fan_written);
    }
    TEST_ASSERT_EQUAL_UINT32(0UL, sink.write_calls);
    TEST_ASSERT_FALSE(sink.pin26_level_high);
    TEST_ASSERT_FALSE(sink.pin25_level_high);
}

void test_lamp_and_fan_hysteresis_transitions_and_pin_independence(void) {
    FakeGpioSink sink;
    AutomationController automation;
    NodeActuatorSync sync;
    sync.beginSafeOff(sink);

    // 1. Inside hysteresis at boot (100.0 lux, 28.0 C) -> stay OFF, 0 GPIO writes.
    automation.evaluateSensors(SensorReading{28.0f, true, 100.0f, true});
    ActuatorSyncOutcome out = sync.sync(automation.state(), sink);
    TEST_ASSERT_FALSE(out.changed);
    TEST_ASSERT_EQUAL_UINT32(0UL, sink.write_calls);

    automation.evaluateSensors(SensorReading{29.9f, true, 150.0f, true});
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_FALSE(out.changed);
    TEST_ASSERT_EQUAL_UINT32(0UL, sink.write_calls);

    // 2. Dark (<100 lux = 99.9) while temp stays 29.9 -> ONLY lamp GPIO26 writes HIGH; GPIO25 untouched!
    automation.evaluateSensors(SensorReading{29.9f, true, 99.9f, true});
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_TRUE(out.changed);
    TEST_ASSERT_TRUE(out.lamp_written);
    TEST_ASSERT_FALSE(out.fan_written);
    TEST_ASSERT_TRUE(sink.pin26_level_high);
    TEST_ASSERT_FALSE(sink.pin25_level_high);
    TEST_ASSERT_EQUAL_UINT32(1UL, sink.pin26_writes);
    TEST_ASSERT_EQUAL_UINT32(0UL, sink.pin25_writes);

    // 3. Re-entering deadband [100, 150] holds lamp ON with 0 writes.
    automation.evaluateSensors(SensorReading{29.9f, true, 100.0f, true});
    TEST_ASSERT_FALSE(sync.sync(automation.state(), sink).changed);
    automation.evaluateSensors(SensorReading{29.9f, true, 150.0f, true});
    TEST_ASSERT_FALSE(sync.sync(automation.state(), sink).changed);
    TEST_ASSERT_EQUAL_UINT32(1UL, sink.pin26_writes);

    // 4. Hot (>=30.0 C = 30.0) while lux stays 150.0 -> ONLY fan GPIO25 writes HIGH; GPIO26 untouched!
    automation.evaluateSensors(SensorReading{30.0f, true, 150.0f, true});
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_TRUE(out.changed);
    TEST_ASSERT_FALSE(out.lamp_written);
    TEST_ASSERT_TRUE(out.fan_written);
    TEST_ASSERT_TRUE(sink.pin26_level_high);
    TEST_ASSERT_TRUE(sink.pin25_level_high);
    TEST_ASSERT_EQUAL_UINT32(1UL, sink.pin26_writes);
    TEST_ASSERT_EQUAL_UINT32(1UL, sink.pin25_writes);

    // 5. Cooling into [28.0, 29.9] holds fan ON with 0 writes.
    automation.evaluateSensors(SensorReading{29.9f, true, 150.0f, true});
    TEST_ASSERT_FALSE(sync.sync(automation.state(), sink).changed);
    automation.evaluateSensors(SensorReading{28.0f, true, 150.0f, true});
    TEST_ASSERT_FALSE(sync.sync(automation.state(), sink).changed);
    TEST_ASSERT_EQUAL_UINT32(1UL, sink.pin25_writes);

    // 6. Bright (>150 lux = 150.1) turns ONLY lamp OFF; fan stays HIGH.
    automation.evaluateSensors(SensorReading{28.0f, true, 150.1f, true});
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_TRUE(out.lamp_written);
    TEST_ASSERT_FALSE(out.fan_written);
    TEST_ASSERT_FALSE(sink.pin26_level_high);
    TEST_ASSERT_TRUE(sink.pin25_level_high);
    TEST_ASSERT_EQUAL_UINT32(2UL, sink.pin26_writes);
    TEST_ASSERT_EQUAL_UINT32(1UL, sink.pin25_writes);

    // 7. Cool (<28.0 C = 27.9) turns ONLY fan OFF; lamp stays LOW.
    automation.evaluateSensors(SensorReading{27.9f, true, 125.0f, true});
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_FALSE(out.lamp_written);
    TEST_ASSERT_TRUE(out.fan_written);
    TEST_ASSERT_FALSE(sink.pin26_level_high);
    TEST_ASSERT_FALSE(sink.pin25_level_high);
    TEST_ASSERT_EQUAL_UINT32(2UL, sink.pin26_writes);
    TEST_ASSERT_EQUAL_UINT32(2UL, sink.pin25_writes);
}

void test_invalid_nan_and_stale_sensor_readings_never_drive_gpio_transitions(void) {
    FakeGpioSink sink;
    AutomationController automation;
    NodeActuatorSync sync;
    sync.beginSafeOff(sink);

    TempHumiditySensor dht(TempHumidityConfig{DhtSensorKind::DHT11, 2000UL, 6000UL});
    LightSensor light(LightSensorConfig{LightSensorKind::BH1750, 1000UL, 5000UL, 150000.0f});
    StubDhtReader dht_reader;
    StubLightReader light_reader;

    // 1. Boot with READ_ERROR on both sensors -> fan and lamp MUST remain OFF (LOW).
    dht_reader.next_io_ok = false;
    dht_reader.next_temp_c = 45.0f;  // Invalid must be ignored
    light_reader.next_status = LightRawStatus::IO_FAILURE;
    light_reader.next_lux = 10.0f;   // Invalid must be ignored

    DhtPollOutcome d0 = dht.poll(0UL, dht_reader);
    LightPollOutcome l0 = light.poll(0UL, light_reader);
    automation.evaluateSensors(
        SensorReading{d0.reading.temperature_c, d0.reading.valid, l0.reading.lux, l0.reading.valid});
    ActuatorSyncOutcome out = sync.sync(automation.state(), sink);
    TEST_ASSERT_FALSE(out.changed);
    TEST_ASSERT_FALSE(sink.pin26_level_high);
    TEST_ASSERT_FALSE(sink.pin25_level_high);
    TEST_ASSERT_EQUAL_UINT32(0UL, sink.write_calls);

    // 2. Valid hot & dark reading turns both ON.
    dht_reader.next_io_ok = true;
    dht_reader.next_temp_c = 32.0f;
    light_reader.next_status = LightRawStatus::OK;
    light_reader.next_lux = 50.0f;
    d0 = dht.poll(2000UL, dht_reader);
    l0 = light.poll(2000UL, light_reader);
    automation.evaluateSensors(
        SensorReading{d0.reading.temperature_c, d0.reading.valid, l0.reading.lux, l0.reading.valid});
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_TRUE(sink.pin26_level_high);
    TEST_ASSERT_TRUE(sink.pin25_level_high);
    TEST_ASSERT_EQUAL_UINT32(2UL, sink.write_calls);

    // 3. Both sensors become STALE without polling -> existing policy holds states, 0 spurious writes.
    dht.refreshStaleness(9000UL);
    light.refreshStaleness(9000UL);
    automation.evaluateSensors(SensorReading{
        dht.reading().temperature_c, dht.reading().valid, light.reading().lux, light.reading().valid});
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::FAULT),
                            static_cast<uint8_t>(automation.state().sensor_health));
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_FALSE(out.changed);
    TEST_ASSERT_TRUE(sink.pin26_level_high);
    TEST_ASSERT_TRUE(sink.pin25_level_high);
    TEST_ASSERT_EQUAL_UINT32(2UL, sink.write_calls);

    // 4. Direct NaN values with valid=true are also rejected safely by AutomationController.
    const float nan_val = std::numeric_limits<float>::quiet_NaN();
    automation.evaluateSensors(SensorReading{nan_val, true, nan_val, true});
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_FALSE(out.changed);
    TEST_ASSERT_EQUAL_UINT32(2UL, sink.write_calls);
}

void test_manual_mode_sync_and_polarity_helper(void) {
    FakeGpioSink sink;
    AutomationController automation;
    NodeActuatorSync sync;
    sync.beginSafeOff(sink);

    // MANUAL turns lamp ON and fan OFF; sensor AUTO conditions must NOT override MANUAL.
    automation.setLampManual(true);
    ActuatorSyncOutcome out = sync.sync(automation.state(), sink);
    TEST_ASSERT_TRUE(out.lamp_written);
    TEST_ASSERT_FALSE(out.fan_written);
    TEST_ASSERT_TRUE(sink.pin26_level_high);
    TEST_ASSERT_FALSE(sink.pin25_level_high);

    automation.evaluateSensors(SensorReading{35.0f, true, 300.0f, true});
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_FALSE(out.changed);
    TEST_ASSERT_TRUE(sink.pin26_level_high);
    TEST_ASSERT_FALSE(sink.pin25_level_high);

    automation.setFanManual(true);
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_FALSE(out.lamp_written);
    TEST_ASSERT_TRUE(out.fan_written);
    TEST_ASSERT_TRUE(sink.pin25_level_high);

    // Switching back to AUTO applies bright/cool sensor readings on next evaluation.
    automation.setModeFromString("AUTO");
    automation.evaluateSensors(SensorReading{25.0f, true, 200.0f, true});
    out = sync.sync(automation.state(), sink);
    TEST_ASSERT_TRUE(out.lamp_written);
    TEST_ASSERT_TRUE(out.fan_written);
    TEST_ASSERT_FALSE(sink.pin26_level_high);
    TEST_ASSERT_FALSE(sink.pin25_level_high);

    // Verify polarity helper for both ACTIVE_HIGH and ACTIVE_LOW.
    TEST_ASSERT_FALSE(NodeActuatorSync::logicalToGpioHigh(false, ActuatorPolarity::ACTIVE_HIGH));
    TEST_ASSERT_TRUE(NodeActuatorSync::logicalToGpioHigh(true, ActuatorPolarity::ACTIVE_HIGH));
    TEST_ASSERT_TRUE(NodeActuatorSync::logicalToGpioHigh(false, ActuatorPolarity::ACTIVE_LOW));
    TEST_ASSERT_FALSE(NodeActuatorSync::logicalToGpioHigh(true, ActuatorPolarity::ACTIVE_LOW));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_boot_safe_off_and_no_redundant_writes_on_unchanged_state);
    RUN_TEST(test_lamp_and_fan_hysteresis_transitions_and_pin_independence);
    RUN_TEST(test_invalid_nan_and_stale_sensor_readings_never_drive_gpio_transitions);
    RUN_TEST(test_manual_mode_sync_and_polarity_helper);
    return UNITY_END();
}
