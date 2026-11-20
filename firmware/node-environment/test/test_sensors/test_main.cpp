#include <unity.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "node_automation.h"
#include "temp_humidity_sensor.h"

using node_env::AutomationController;
using node_env::AutomationState;
using node_env::DhtPollOutcome;
using node_env::DhtReadError;
using node_env::DhtSensorKind;
using node_env::DhtSensorLimits;
using node_env::IDhtRawReader;
using node_env::SensorHealth;
using node_env::SensorReading;
using node_env::TempHumidityConfig;
using node_env::TempHumiditySensor;
using node_env::TempHumidityStatus;

namespace {

class StubDhtReader final : public IDhtRawReader {
public:
    bool next_io_ok = true;
    float next_temp_c = 25.0f;
    float next_humidity_pct = 55.0f;
    uint32_t call_count = 0UL;

    bool readRawSample(float* out_temp_c, float* out_humidity_pct) override {
        ++call_count;
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

void test_initial_uninitialized_state_and_config_sanitization(void) {
    TempHumidityConfig bad_cfg;
    bad_cfg.sensor_kind = static_cast<DhtSensorKind>(99);
    bad_cfg.sample_interval_ms = 500UL;  // Below mandatory 2000 ms floor
    bad_cfg.stale_timeout_ms = 0UL;

    TempHumiditySensor sensor(bad_cfg);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtSensorKind::DHT11),
                            static_cast<uint8_t>(sensor.config().sensor_kind));
    TEST_ASSERT_EQUAL_UINT32(2000UL, sensor.config().sample_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(6000UL, sensor.config().stale_timeout_ms);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TempHumidityStatus::UNINITIALIZED),
                            static_cast<uint8_t>(sensor.status()));
    TEST_ASSERT_FALSE(sensor.reading().valid);
    TEST_ASSERT_TRUE(std::isnan(sensor.reading().temperature_c));
    TEST_ASSERT_TRUE(std::isnan(sensor.reading().humidity_pct));
    TEST_ASSERT_TRUE(sensor.isSampleDue(0UL));
}

void test_first_sample_cadence_and_millis_rollover(void) {
    StubDhtReader reader;
    reader.next_io_ok = true;
    reader.next_temp_c = 28.5f;
    reader.next_humidity_pct = 62.0f;

    TempHumidityConfig cfg;
    cfg.sensor_kind = DhtSensorKind::DHT11;
    cfg.sample_interval_ms = 2000UL;
    cfg.stale_timeout_ms = 6000UL;
    TempHumiditySensor sensor(cfg);

    // First sample at boot (even at now_ms == 0) executes immediately.
    DhtPollOutcome first = sensor.poll(0UL, reader);
    TEST_ASSERT_TRUE(first.attempted);
    TEST_ASSERT_TRUE(first.status_changed);
    TEST_ASSERT_EQUAL_UINT32(1UL, reader.call_count);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TempHumidityStatus::VALID),
                            static_cast<uint8_t>(first.reading.status));
    TEST_ASSERT_TRUE(first.reading.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 28.5f, first.reading.temperature_c);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 62.0f, first.reading.humidity_pct);

    // Within 1999 ms: must NOT hit hardware reader again.
    DhtPollOutcome too_early = sensor.poll(1999UL, reader);
    TEST_ASSERT_FALSE(too_early.attempted);
    TEST_ASSERT_FALSE(too_early.status_changed);
    TEST_ASSERT_EQUAL_UINT32(1UL, reader.call_count);

    // At exact 2000 ms boundary: sample executes.
    reader.next_temp_c = 29.0f;
    DhtPollOutcome second = sensor.poll(2000UL, reader);
    TEST_ASSERT_TRUE(second.attempted);
    TEST_ASSERT_FALSE(second.status_changed);
    TEST_ASSERT_EQUAL_UINT32(2UL, reader.call_count);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 29.0f, second.reading.temperature_c);

    // Jump forward to just before uint32_t rollover; sample at 0xFFFFFE0CUL (UINT32_MAX - 499).
    const uint32_t pre_wrap_ms = 0xFFFFFE0CUL;
    reader.next_temp_c = 30.2f;
    DhtPollOutcome wrap_base = sensor.poll(pre_wrap_ms, reader);
    TEST_ASSERT_TRUE(wrap_base.attempted);
    TEST_ASSERT_EQUAL_UINT32(3UL, reader.call_count);

    // 1999 ms elapsed across rollover: pre_wrap_ms + 1999 = 0x000005DBUL -> must NOT sample.
    const uint32_t post_wrap_1999_ms = pre_wrap_ms + 1999UL;
    DhtPollOutcome wrap_early = sensor.poll(post_wrap_1999_ms, reader);
    TEST_ASSERT_FALSE(wrap_early.attempted);
    TEST_ASSERT_EQUAL_UINT32(3UL, reader.call_count);

    // 2000 ms elapsed across rollover: pre_wrap_ms + 2000 = 0x000005DCUL -> MUST sample.
    const uint32_t post_wrap_2000_ms = pre_wrap_ms + 2000UL;
    reader.next_temp_c = 31.4f;
    DhtPollOutcome wrap_due = sensor.poll(post_wrap_2000_ms, reader);
    TEST_ASSERT_TRUE(wrap_due.attempted);
    TEST_ASSERT_EQUAL_UINT32(4UL, reader.call_count);
    TEST_ASSERT_TRUE(wrap_due.reading.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 31.4f, wrap_due.reading.temperature_c);
}

void test_nan_inf_and_sensor_specific_range_boundaries(void) {
    const float nan_val = std::numeric_limits<float>::quiet_NaN();
    const float pos_inf = std::numeric_limits<float>::infinity();
    const float neg_inf = -std::numeric_limits<float>::infinity();
    DhtReadError err = DhtReadError::NONE;

    // Non-finite and I/O failures rejected for both sensors.
    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT11, 25.0f, 50.0f, false, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::IO_FAILURE), static_cast<uint8_t>(err));

    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT11, nan_val, 50.0f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::NON_FINITE_VALUE), static_cast<uint8_t>(err));

    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT22, 25.0f, nan_val, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::NON_FINITE_VALUE), static_cast<uint8_t>(err));

    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT22, pos_inf, 50.0f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::NON_FINITE_VALUE), static_cast<uint8_t>(err));

    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT11, 25.0f, neg_inf, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::NON_FINITE_VALUE), static_cast<uint8_t>(err));

    // DHT11 exact limits: Temp [0.0, 50.0] C, Humidity [20.0, 80.0] % RH.
    const DhtSensorLimits dht11_limits = TempHumiditySensor::sensorLimits(DhtSensorKind::DHT11);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, dht11_limits.min_temp_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, dht11_limits.max_temp_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, dht11_limits.min_humidity_pct);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 80.0f, dht11_limits.max_humidity_pct);

    TEST_ASSERT_TRUE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT11, 0.0f, 20.0f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::NONE), static_cast<uint8_t>(err));
    TEST_ASSERT_TRUE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT11, 50.0f, 80.0f, true, &err));

    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT11, -0.1f, 50.0f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::TEMPERATURE_OUT_OF_RANGE),
                            static_cast<uint8_t>(err));
    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT11, 50.1f, 50.0f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::TEMPERATURE_OUT_OF_RANGE),
                            static_cast<uint8_t>(err));
    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT11, 25.0f, 19.9f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::HUMIDITY_OUT_OF_RANGE),
                            static_cast<uint8_t>(err));
    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT11, 25.0f, 80.1f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::HUMIDITY_OUT_OF_RANGE),
                            static_cast<uint8_t>(err));

    // DHT22 exact limits: Temp [-40.0, 80.0] C, Humidity [0.0, 100.0] % RH.
    const DhtSensorLimits dht22_limits = TempHumiditySensor::sensorLimits(DhtSensorKind::DHT22);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -40.0f, dht22_limits.min_temp_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 80.0f, dht22_limits.max_temp_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, dht22_limits.min_humidity_pct);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, dht22_limits.max_humidity_pct);

    TEST_ASSERT_TRUE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT22, -40.0f, 0.0f, true, &err));
    TEST_ASSERT_TRUE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT22, 80.0f, 100.0f, true, &err));

    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT22, -40.1f, 50.0f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::TEMPERATURE_OUT_OF_RANGE),
                            static_cast<uint8_t>(err));
    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT22, 80.1f, 50.0f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::TEMPERATURE_OUT_OF_RANGE),
                            static_cast<uint8_t>(err));
    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT22, 25.0f, -0.1f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::HUMIDITY_OUT_OF_RANGE),
                            static_cast<uint8_t>(err));
    TEST_ASSERT_FALSE(TempHumiditySensor::validateRawSample(DhtSensorKind::DHT22, 25.0f, 100.1f, true, &err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::HUMIDITY_OUT_OF_RANGE),
                            static_cast<uint8_t>(err));
}

void test_stale_expiration_with_rollover_and_recovery(void) {
    StubDhtReader reader;
    reader.next_io_ok = true;
    reader.next_temp_c = 24.5f;
    reader.next_humidity_pct = 55.0f;

    // Configure sample interval > stale timeout to verify VALID -> STALE between polls.
    TempHumidityConfig cfg;
    cfg.sensor_kind = DhtSensorKind::DHT22;
    cfg.sample_interval_ms = 10000UL;
    cfg.stale_timeout_ms = 6000UL;
    TempHumiditySensor sensor(cfg);

    const uint32_t base_ms = 0xFFFFFA00UL;  // Wrap-around test
    DhtPollOutcome initial = sensor.poll(base_ms, reader);
    TEST_ASSERT_TRUE(initial.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TempHumidityStatus::VALID),
                            static_cast<uint8_t>(initial.reading.status));

    // Exact boundary (6000 ms elapsed across rollover): still VALID.
    DhtPollOutcome boundary = sensor.poll(base_ms + 6000UL, reader);
    TEST_ASSERT_FALSE(boundary.attempted);
    TEST_ASSERT_FALSE(boundary.status_changed);
    TEST_ASSERT_TRUE(boundary.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TempHumidityStatus::VALID),
                            static_cast<uint8_t>(boundary.reading.status));

    // Past timeout (6001 ms elapsed): becomes STALE and invalidates cached values (never 0 or stale temp).
    DhtPollOutcome stale = sensor.poll(base_ms + 6001UL, reader);
    TEST_ASSERT_FALSE(stale.attempted);
    TEST_ASSERT_TRUE(stale.status_changed);
    TEST_ASSERT_FALSE(stale.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TempHumidityStatus::STALE),
                            static_cast<uint8_t>(stale.reading.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::STALE_TIMEOUT),
                            static_cast<uint8_t>(stale.reading.error));
    TEST_ASSERT_TRUE(std::isnan(stale.reading.temperature_c));
    TEST_ASSERT_TRUE(std::isnan(stale.reading.humidity_pct));

    // Repeated call before next sample stays STALE with status_changed=false.
    DhtPollOutcome still_stale = sensor.poll(base_ms + 8000UL, reader);
    TEST_ASSERT_FALSE(still_stale.status_changed);
    TEST_ASSERT_FALSE(still_stale.reading.valid);

    // Next scheduled sample succeeds -> recovers cleanly to VALID.
    reader.next_temp_c = 26.0f;
    reader.next_humidity_pct = 58.0f;
    DhtPollOutcome recovered = sensor.poll(base_ms + 10000UL, reader);
    TEST_ASSERT_TRUE(recovered.attempted);
    TEST_ASSERT_TRUE(recovered.status_changed);
    TEST_ASSERT_TRUE(recovered.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TempHumidityStatus::VALID),
                            static_cast<uint8_t>(recovered.reading.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::NONE),
                            static_cast<uint8_t>(recovered.reading.error));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 26.0f, recovered.reading.temperature_c);
}

void test_read_error_never_reuses_old_sample_and_automation_stays_safe(void) {
    StubDhtReader reader;
    TempHumidityConfig cfg;
    cfg.sensor_kind = DhtSensorKind::DHT11;
    cfg.sample_interval_ms = 2000UL;
    cfg.stale_timeout_ms = 6000UL;
    TempHumiditySensor sensor(cfg);
    AutomationController automation;

    // 1. Valid hot sample (31.0 C) turns fan ON while light_lux_valid remains false -> DEGRADED.
    reader.next_io_ok = true;
    reader.next_temp_c = 31.0f;
    reader.next_humidity_pct = 60.0f;
    DhtPollOutcome o1 = sensor.poll(1000UL, reader);
    TEST_ASSERT_TRUE(o1.reading.valid);

    SensorReading r1{o1.reading.temperature_c, o1.reading.valid, 0.0f, false};
    AutomationState s1 = automation.evaluateSensors(r1);
    TEST_ASSERT_TRUE(s1.fan_on);
    TEST_ASSERT_FALSE(s1.lamp_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::DEGRADED),
                            static_cast<uint8_t>(s1.sensor_health));

    // 2. Subsequent I/O failure must mark valid=false, clear numeric values to NaN (not 0 or 31.0),
    //    and preserve fan_on=true safely while updating sensor_health=FAULT.
    reader.next_io_ok = false;
    reader.next_temp_c = 0.0f;  // Must NEVER leak 0.0 as a valid sample
    reader.next_humidity_pct = 0.0f;
    DhtPollOutcome o2 = sensor.poll(3000UL, reader);
    TEST_ASSERT_TRUE(o2.attempted);
    TEST_ASSERT_TRUE(o2.status_changed);
    TEST_ASSERT_FALSE(o2.reading.valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TempHumidityStatus::READ_ERROR),
                            static_cast<uint8_t>(o2.reading.status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::IO_FAILURE),
                            static_cast<uint8_t>(o2.reading.error));
    TEST_ASSERT_EQUAL_UINT32(1UL, o2.reading.consecutive_failures);
    TEST_ASSERT_TRUE(std::isnan(o2.reading.temperature_c));
    TEST_ASSERT_TRUE(std::isnan(o2.reading.humidity_pct));

    SensorReading r2{o2.reading.temperature_c, o2.reading.valid, 0.0f, false};
    AutomationState s2 = automation.evaluateSensors(r2);
    TEST_ASSERT_TRUE(s2.fan_on);  // Held safe; NOT turned OFF by invalid/0 reading
    TEST_ASSERT_FALSE(s2.lamp_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::FAULT),
                            static_cast<uint8_t>(s2.sensor_health));

    // 3. Out-of-range DHT11 temperature (e.g. -5.0 C) increments failures and keeps reading invalid.
    reader.next_io_ok = true;
    reader.next_temp_c = -5.0f;
    reader.next_humidity_pct = 50.0f;
    DhtPollOutcome o3 = sensor.poll(5000UL, reader);
    TEST_ASSERT_FALSE(o3.reading.valid);
    TEST_ASSERT_EQUAL_UINT32(2UL, o3.reading.consecutive_failures);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DhtReadError::TEMPERATURE_OUT_OF_RANGE),
                            static_cast<uint8_t>(o3.reading.error));
    TEST_ASSERT_TRUE(std::isnan(o3.reading.temperature_c));

    // Between polls (t=5500), failed reading stays READ_ERROR/invalid (never reverts to old 31.0 C).
    DhtPollOutcome between = sensor.poll(5500UL, reader);
    TEST_ASSERT_FALSE(between.attempted);
    TEST_ASSERT_FALSE(between.reading.valid);
    TEST_ASSERT_TRUE(std::isnan(between.reading.temperature_c));

    // 4. Recovery with cool valid reading (26.0 C) resets failures and turns fan OFF in AUTO.
    reader.next_io_ok = true;
    reader.next_temp_c = 26.0f;
    reader.next_humidity_pct = 55.0f;
    DhtPollOutcome o4 = sensor.poll(7000UL, reader);
    TEST_ASSERT_TRUE(o4.attempted);
    TEST_ASSERT_TRUE(o4.status_changed);
    TEST_ASSERT_TRUE(o4.reading.valid);
    TEST_ASSERT_EQUAL_UINT32(0UL, o4.reading.consecutive_failures);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TempHumidityStatus::VALID),
                            static_cast<uint8_t>(o4.reading.status));

    SensorReading r4{o4.reading.temperature_c, o4.reading.valid, 0.0f, false};
    AutomationState s4 = automation.evaluateSensors(r4);
    TEST_ASSERT_FALSE(s4.fan_on);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorHealth::DEGRADED),
                            static_cast<uint8_t>(s4.sensor_health));
}

void test_serial_log_rate_limiter_respects_intervals_and_rollover(void) {
    // First log emits immediately.
    TEST_ASSERT_TRUE(TempHumiditySensor::shouldEmitSerialLog(false, 10UL, 0UL, false, 5000UL, 2000UL));

    // Steady-state suppressed before 5000 ms, allowed at 5000 ms.
    TEST_ASSERT_FALSE(TempHumiditySensor::shouldEmitSerialLog(true, 4999UL, 0UL, false, 5000UL, 2000UL));
    TEST_ASSERT_TRUE(TempHumiditySensor::shouldEmitSerialLog(true, 5000UL, 0UL, false, 5000UL, 2000UL));

    // Status change allowed once >= 2000 ms elapsed, suppressed below 2000 ms.
    TEST_ASSERT_FALSE(TempHumiditySensor::shouldEmitSerialLog(true, 6999UL, 5000UL, true, 5000UL, 2000UL));
    TEST_ASSERT_TRUE(TempHumiditySensor::shouldEmitSerialLog(true, 7000UL, 5000UL, true, 5000UL, 2000UL));

    // Rollover check across UINT32_MAX.
    const uint32_t last_ms = 0xFFFFF800UL;
    TEST_ASSERT_FALSE(TempHumiditySensor::shouldEmitSerialLog(
        true, last_ms + 4999UL, last_ms, false, 5000UL, 2000UL));
    TEST_ASSERT_TRUE(TempHumiditySensor::shouldEmitSerialLog(
        true, last_ms + 5000UL, last_ms, false, 5000UL, 2000UL));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_initial_uninitialized_state_and_config_sanitization);
    RUN_TEST(test_first_sample_cadence_and_millis_rollover);
    RUN_TEST(test_nan_inf_and_sensor_specific_range_boundaries);
    RUN_TEST(test_stale_expiration_with_rollover_and_recovery);
    RUN_TEST(test_read_error_never_reuses_old_sample_and_automation_stays_safe);
    RUN_TEST(test_serial_log_rate_limiter_respects_intervals_and_rollover);
    return UNITY_END();
}
