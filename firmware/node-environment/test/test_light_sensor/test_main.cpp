#include <unity.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "light_sensor.h"
#include "node_automation.h"
#include "temp_humidity_sensor.h"

using node_env::AutomationController;
using node_env::AutomationState;
using node_env::DhtPollOutcome;
using node_env::DhtSensorKind;
using node_env::IDhtRawReader;
using node_env::ILightRawReader;
using node_env::LightPollOutcome;
using node_env::LightRawStatus;
using node_env::LightReadError;
using node_env::LightSensor;
using node_env::LightSensorConfig;
using node_env::LightSensorKind;
using node_env::LightSensorStatus;
using node_env::SensorHealth;
using node_env::SensorReading;
using node_env::TempHumidityConfig;
using node_env::TempHumiditySensor;
using node_env::TempHumidityStatus;
using node_env::WokwiLdrConversionParams;

namespace {

class StubLightReader final : public ILightRawReader {
public:
    LightRawStatus next_status = LightRawStatus::OK;
    float next_lux = 120.0f;
    uint32_t call_count = 0UL;

    LightRawStatus readRawLux(float* out_lux) override {
        ++call_count;
        if (out_lux != nullptr) {
            *out_lux = next_lux;
        }
        return next_status;
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

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_light_initial_state_and_config_sanitization(void) {
    LightSensorConfig bad_cfg;
    bad_cfg.sensor_kind = static_cast<LightSensorKind>(77);
    bad_cfg.sample_interval_ms = 10UL;  // Below 200 ms floor -> falls back to 1000 ms
    bad_cfg.stale_timeout_ms = 0UL;
    bad_cfg.max_valid_lux = -10.0f;

    LightSensor sensor(bad_cfg);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightSensorKind::BH1750),
                            static_cast<uint8_t>(sensor.config().sensor_kind));
    TEST_ASSERT_EQUAL_UINT32(1000UL, sensor.config().sample_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(5000UL, sensor.config().stale_timeout_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 150000.0f, sensor.config().max_valid_lux);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightSensorStatus::UNINITIALIZED),
                            static_cast<uint8_t>(sensor.status()));
    TEST_ASSERT_FALSE(sensor.reading().valid);
    TEST_ASSERT_FALSE(sensor.reading().is_simulated_estimate);
    TEST_ASSERT_TRUE(std::isnan(sensor.reading().lux));
    TEST_ASSERT_TRUE(sensor.isSampleDue(0UL));

    LightSensorConfig sim_cfg;
    sim_cfg.sensor_kind = LightSensorKind::LDR_ADC_SIM;
    LightSensor sim_sensor(sim_cfg);
    TEST_ASSERT_TRUE(sim_sensor.reading().is_simulated_estimate);
}

void test_light_sampling_cadence_and_millis_rollover(void) {
    StubLightReader reader;
    reader.next_status = LightRawStatus::OK;
    reader.next_lux = 250.0f;

    LightSensorConfig cfg;
    cfg.sensor_kind = LightSensorKind::BH1750;
    cfg.sample_interval_ms = 1000UL;
    cfg.stale_timeout_ms = 5000UL;
    LightSensor sensor(cfg);

    LightPollOutcome first = sensor.poll(0UL, reader);
    TEST_ASSERT_TRUE(first.attempted);
    TEST_ASSERT_TRUE(first.status_changed);
    TEST_ASSERT_EQUAL_UINT32(1UL, reader.call_count);
    TEST_ASSERT_TRUE(first.reading.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 250.0f, first.reading.lux);

    // Within 999 ms: must NOT sample again.
    LightPollOutcome early = sensor.poll(999UL, reader);
    TEST_ASSERT_FALSE(early.attempted);
    TEST_ASSERT_EQUAL_UINT32(1UL, reader.call_count);

    // Jump near uint32_t rollover: UINT32_MAX - 399 = 0xFFFFFE70UL.
    const uint32_t pre_wrap_ms = 0xFFFFFE70UL;
    reader.next_lux = 180.0f;
    LightPollOutcome base = sensor.poll(pre_wrap_ms, reader);
    TEST_ASSERT_TRUE(base.attempted);
    TEST_ASSERT_EQUAL_UINT32(2UL, reader.call_count);

    // 999 ms across wraparound -> NOT due.
    LightPollOutcome wrap_early = sensor.poll(pre_wrap_ms + 999UL, reader);
    TEST_ASSERT_FALSE(wrap_early.attempted);
    TEST_ASSERT_EQUAL_UINT32(2UL, reader.call_count);

    // 1000 ms across wraparound -> DUE.
    reader.next_lux = 90.0f;
    LightPollOutcome wrap_due = sensor.poll(pre_wrap_ms + 1000UL, reader);
    TEST_ASSERT_TRUE(wrap_due.attempted);
    TEST_ASSERT_EQUAL_UINT32(3UL, reader.call_count);
    TEST_ASSERT_TRUE(wrap_due.reading.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, wrap_due.reading.lux);
}

void test_bh1750_errors_not_ready_disconnect_and_recovery(void) {
    StubLightReader reader;
    LightSensorConfig cfg;
    cfg.sensor_kind = LightSensorKind::BH1750;
    cfg.sample_interval_ms = 1000UL;
    cfg.stale_timeout_ms = 5000UL;
    LightSensor sensor(cfg);

    // 1. Initial conversion not ready -> remains UNINITIALIZED with valid=false (not READ_ERROR).
    reader.next_status = LightRawStatus::MEASUREMENT_NOT_READY;
    reader.next_lux = 0.0f;
    LightPollOutcome warmup = sensor.poll(10UL, reader);
    TEST_ASSERT_TRUE(warmup.attempted);
    TEST_ASSERT_FALSE(warmup.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightSensorStatus::UNINITIALIZED),
                            static_cast<uint8_t>(warmup.reading.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightReadError::MEASUREMENT_NOT_READY),
                            static_cast<uint8_t>(warmup.reading.error));
    TEST_ASSERT_EQUAL_UINT32(0UL, warmup.reading.consecutive_failures);
    TEST_ASSERT_TRUE(std::isnan(warmup.reading.lux));

    // 2. begin() failed / not initialized -> READ_ERROR with valid=false.
    reader.next_status = LightRawStatus::NOT_INITIALIZED;
    LightPollOutcome no_init = sensor.poll(1010UL, reader);
    TEST_ASSERT_TRUE(no_init.status_changed);
    TEST_ASSERT_FALSE(no_init.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightSensorStatus::READ_ERROR),
                            static_cast<uint8_t>(no_init.reading.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightReadError::NOT_INITIALIZED),
                            static_cast<uint8_t>(no_init.reading.error));
    TEST_ASSERT_EQUAL_UINT32(1UL, no_init.reading.consecutive_failures);

    // 3. Valid sample arrives -> recovers to VALID.
    reader.next_status = LightRawStatus::OK;
    reader.next_lux = 120.5f;
    LightPollOutcome ok = sensor.poll(2010UL, reader);
    TEST_ASSERT_TRUE(ok.status_changed);
    TEST_ASSERT_TRUE(ok.reading.valid);
    TEST_ASSERT_EQUAL_UINT32(0UL, ok.reading.consecutive_failures);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.5f, ok.reading.lux);

    // 4. Reject NaN, +Inf, negative (-1.0f sentinel), and > max_valid_lux without keeping old 120.5f.
    const float bad_values[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -1.0f,
        150001.0f,
    };
    uint32_t t_ms = 3010UL;
    for (size_t i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]); ++i) {
        reader.next_status = LightRawStatus::OK;
        reader.next_lux = bad_values[i];
        LightPollOutcome err_out = sensor.poll(t_ms, reader);
        TEST_ASSERT_FALSE(err_out.reading.valid);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightSensorStatus::READ_ERROR),
                                static_cast<uint8_t>(err_out.reading.status));
        TEST_ASSERT_TRUE(std::isnan(err_out.reading.lux));
        t_ms += 1000UL;
    }

    // 5. I2C disconnection -> IO_FAILURE, then recovery -> VALID.
    reader.next_status = LightRawStatus::IO_FAILURE;
    reader.next_lux = 0.0f;
    LightPollOutcome io_err = sensor.poll(t_ms, reader);
    TEST_ASSERT_FALSE(io_err.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightReadError::IO_FAILURE),
                            static_cast<uint8_t>(io_err.reading.error));
    TEST_ASSERT_TRUE(std::isnan(io_err.reading.lux));

    reader.next_status = LightRawStatus::OK;
    reader.next_lux = 0.0f;  // 0.0 lux is valid ONLY when IO_OK (complete darkness on BH1750)
    LightPollOutcome dark_ok = sensor.poll(t_ms + 1000UL, reader);
    TEST_ASSERT_TRUE(dark_ok.reading.valid);
    TEST_ASSERT_EQUAL_UINT32(0UL, dark_ok.reading.consecutive_failures);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, dark_ok.reading.lux);
}

void test_wokwi_ldr_adc_formula_polarity_and_extreme_guards(void) {
    WokwiLdrConversionParams params;
    float lux = 0.0f;
    LightReadError err = LightReadError::NONE;

    // 1. Verify exact 10 lux calibration point: at 10 lux, R_ldr == 50k, R_fixed == 10k -> ratio 5/6 -> ADC ~3412.5.
    const int32_t adc_10lux = 3412;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::OK),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(adc_10lux, params, 150000.0f, &lux, &err)));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 10.0f, lux);

    // 2. Verify monotonic polarity: brighter light => lower R_ldr => LOWER AO ADC => HIGHER lux.
    float lux_dark = 0.0f;
    float lux_mid = 0.0f;
    float lux_bright = 0.0f;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::OK),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(3000, params, 150000.0f, &lux_dark, &err)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::OK),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(2045, params, 150000.0f, &lux_mid, &err)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::OK),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(680, params, 150000.0f, &lux_bright, &err)));
    TEST_ASSERT_TRUE(lux_dark < 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 100.0f, lux_mid);
    TEST_ASSERT_TRUE(lux_bright > 150.0f);
    TEST_ASSERT_TRUE(lux_dark < lux_mid && lux_mid < lux_bright);

    // 3. Reject ADC rail extremes (0 and 4095) and out-of-range (<0 and >4095).
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::ADC_EXTREME_RAIL),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(0, params, 150000.0f, &lux, &err)));
    TEST_ASSERT_TRUE(std::isnan(lux));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightReadError::ADC_EXTREME_RAIL),
                            static_cast<uint8_t>(err));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::ADC_EXTREME_RAIL),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(4095, params, 150000.0f, &lux, &err)));
    TEST_ASSERT_TRUE(std::isnan(lux));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::ADC_OUT_OF_RANGE),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(-1, params, 150000.0f, &lux, &err)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::ADC_OUT_OF_RANGE),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(4096, params, 150000.0f, &lux, &err)));

    // Near-short ADC=1 exceeds 150,000 lux upper ceiling -> rejected cleanly.
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::LUX_OUT_OF_RANGE),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(1, params, 150000.0f, &lux, &err)));
    TEST_ASSERT_TRUE(std::isnan(lux));

    // Invalid conversion parameters (zero gamma / resistor) must never divide by zero.
    WokwiLdrConversionParams bad_params;
    bad_params.gamma = 0.0f;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LightRawStatus::IO_FAILURE),
        static_cast<uint8_t>(LightSensor::convertWokwiLdrAdcToLux(2045, bad_params, 150000.0f, &lux, &err)));
}

void test_light_stale_expiration_and_recovery(void) {
    StubLightReader reader;
    reader.next_status = LightRawStatus::OK;
    reader.next_lux = 85.0f;

    LightSensorConfig cfg;
    cfg.sensor_kind = LightSensorKind::LDR_ADC_SIM;
    cfg.sample_interval_ms = 8000UL;
    cfg.stale_timeout_ms = 5000UL;
    LightSensor sensor(cfg);

    const uint32_t base_ms = 0xFFFFF000UL;
    LightPollOutcome initial = sensor.poll(base_ms, reader);
    TEST_ASSERT_TRUE(initial.reading.valid);
    TEST_ASSERT_TRUE(initial.reading.is_simulated_estimate);

    // Exact 5000 ms boundary across rollover -> still VALID.
    LightPollOutcome boundary = sensor.poll(base_ms + 5000UL, reader);
    TEST_ASSERT_TRUE(boundary.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightSensorStatus::VALID),
                            static_cast<uint8_t>(boundary.reading.status));

    // 5001 ms -> STALE, valid=false, lux=NaN.
    LightPollOutcome stale = sensor.poll(base_ms + 5001UL, reader);
    TEST_ASSERT_TRUE(stale.status_changed);
    TEST_ASSERT_FALSE(stale.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightSensorStatus::STALE),
                            static_cast<uint8_t>(stale.reading.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LightReadError::STALE_TIMEOUT),
                            static_cast<uint8_t>(stale.reading.error));
    TEST_ASSERT_TRUE(std::isnan(stale.reading.lux));

    // Recovery at next scheduled poll (8000 ms).
    reader.next_lux = 180.0f;
    LightPollOutcome recovered = sensor.poll(base_ms + 8000UL, reader);
    TEST_ASSERT_TRUE(recovered.attempted);
    TEST_ASSERT_TRUE(recovered.status_changed);
    TEST_ASSERT_TRUE(recovered.reading.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, recovered.reading.lux);
}

void test_sensor_fusion_with_dht_and_automation_hysteresis(void) {
    StubDhtReader dht_reader;
    StubLightReader light_reader;

    TempHumiditySensor dht(TempHumidityConfig{DhtSensorKind::DHT22, 2000UL, 6000UL});
    LightSensor light(LightSensorConfig{LightSensorKind::LDR_ADC_SIM, 1000UL, 5000UL, 150000.0f});
    AutomationController automation;

    // 1. Both sensors valid near hysteresis boundaries:
    //    temp=31.0 (>=30 -> fan ON), lux=99.9 (<100 -> lamp ON) => SensorHealth::OK.
    dht_reader.next_io_ok = true;
    dht_reader.next_temp_c = 31.0f;
    dht_reader.next_humidity_pct = 55.0f;
    light_reader.next_status = LightRawStatus::OK;
    light_reader.next_lux = 99.9f;

    DhtPollOutcome d1 = dht.poll(0UL, dht_reader);
    LightPollOutcome l1 = light.poll(0UL, light_reader);
    AutomationState s1 = automation.evaluateSensors(
        SensorReading{d1.reading.temperature_c, d1.reading.valid, l1.reading.lux, l1.reading.valid});
    TEST_ASSERT_TRUE(s1.lamp_on);
    TEST_ASSERT_TRUE(s1.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::OK), static_cast<uint8_t>(s1.sensor_health));

    // 2. Hysteresis deadband [100, 150]: 100.0 and 150.0 keep lamp ON.
    light_reader.next_lux = 100.0f;
    l1 = light.poll(1000UL, light_reader);
    s1 = automation.evaluateSensors(
        SensorReading{d1.reading.temperature_c, d1.reading.valid, l1.reading.lux, l1.reading.valid});
    TEST_ASSERT_TRUE(s1.lamp_on);

    light_reader.next_lux = 150.0f;
    l1 = light.poll(2000UL, light_reader);
    d1 = dht.poll(2000UL, dht_reader);
    s1 = automation.evaluateSensors(
        SensorReading{d1.reading.temperature_c, d1.reading.valid, l1.reading.lux, l1.reading.valid});
    TEST_ASSERT_TRUE(s1.lamp_on);

    // 3. Above 150.0 (150.1) turns lamp OFF, then dropping to 150.0 and 100.0 keeps lamp OFF.
    light_reader.next_lux = 150.1f;
    l1 = light.poll(3000UL, light_reader);
    s1 = automation.evaluateSensors(
        SensorReading{dht.reading().temperature_c, dht.reading().valid, l1.reading.lux, l1.reading.valid});
    TEST_ASSERT_FALSE(s1.lamp_on);
    TEST_ASSERT_TRUE(s1.fan_on);

    // 4. Light fails (ADC rail) while DHT is fresh & cool (26.0 C):
    //    lamp_on stays OFF (unchanged by invalid lux), fan turns OFF from valid DHT, health -> DEGRADED.
    dht_reader.next_temp_c = 26.0f;
    d1 = dht.poll(4000UL, dht_reader);
    light_reader.next_status = LightRawStatus::ADC_EXTREME_RAIL;
    l1 = light.poll(4000UL, light_reader);
    s1 = automation.evaluateSensors(
        SensorReading{d1.reading.temperature_c, d1.reading.valid, l1.reading.lux, l1.reading.valid});
    TEST_ASSERT_FALSE(s1.lamp_on);
    TEST_ASSERT_FALSE(s1.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::DEGRADED),
                            static_cast<uint8_t>(s1.sensor_health));

    // 5. Light recovers at 80.0 lux (lamp turns ON) while DHT fails at t=6000:
    //    fan_on stays OFF (unchanged by invalid DHT), lamp turns ON, health -> DEGRADED.
    dht_reader.next_io_ok = false;
    d1 = dht.poll(6000UL, dht_reader);
    light_reader.next_status = LightRawStatus::OK;
    light_reader.next_lux = 80.0f;
    l1 = light.poll(6000UL, light_reader);
    s1 = automation.evaluateSensors(
        SensorReading{d1.reading.temperature_c, d1.reading.valid, l1.reading.lux, l1.reading.valid});
    TEST_ASSERT_TRUE(s1.lamp_on);
    TEST_ASSERT_FALSE(s1.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::DEGRADED),
                            static_cast<uint8_t>(s1.sensor_health));

    // 6. Both sensors fail -> SensorHealth::FAULT; both lamp_on (true) and fan_on (false) hold safe.
    light_reader.next_status = LightRawStatus::IO_FAILURE;
    l1 = light.poll(7000UL, light_reader);
    s1 = automation.evaluateSensors(
        SensorReading{dht.reading().temperature_c, dht.reading().valid, l1.reading.lux, l1.reading.valid});
    TEST_ASSERT_TRUE(s1.lamp_on);
    TEST_ASSERT_FALSE(s1.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::FAULT),
                            static_cast<uint8_t>(s1.sensor_health));

    // 7. Both recover -> SensorHealth::OK.
    dht_reader.next_io_ok = true;
    dht_reader.next_temp_c = 25.0f;
    d1 = dht.poll(8000UL, dht_reader);
    light_reader.next_status = LightRawStatus::OK;
    light_reader.next_lux = 125.0f;  // Inside [100, 150] -> holds lamp ON
    l1 = light.poll(8000UL, light_reader);
    s1 = automation.evaluateSensors(
        SensorReading{d1.reading.temperature_c, d1.reading.valid, l1.reading.lux, l1.reading.valid});
    TEST_ASSERT_TRUE(s1.lamp_on);
    TEST_ASSERT_FALSE(s1.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::OK),
                            static_cast<uint8_t>(s1.sensor_health));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_light_initial_state_and_config_sanitization);
    RUN_TEST(test_light_sampling_cadence_and_millis_rollover);
    RUN_TEST(test_bh1750_errors_not_ready_disconnect_and_recovery);
    RUN_TEST(test_wokwi_ldr_adc_formula_polarity_and_extreme_guards);
    RUN_TEST(test_light_stale_expiration_and_recovery);
    RUN_TEST(test_sensor_fusion_with_dht_and_automation_hysteresis);
    return UNITY_END();
}
