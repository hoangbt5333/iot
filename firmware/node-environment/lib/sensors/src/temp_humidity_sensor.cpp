#include "temp_humidity_sensor.h"

#include <cmath>
#include <limits>

namespace node_env {

TempHumiditySensor::TempHumiditySensor() : TempHumiditySensor(TempHumidityConfig{}) {}

TempHumiditySensor::TempHumiditySensor(const TempHumidityConfig& config) {
    sanitizeConfig(config);
    reading_.sensor_kind = config_.sensor_kind;
    invalidateReading(TempHumidityStatus::UNINITIALIZED, DhtReadError::NONE);
}

void TempHumiditySensor::sanitizeConfig(const TempHumidityConfig& raw_config) {
    config_.sensor_kind = (raw_config.sensor_kind == DhtSensorKind::DHT22)
                              ? DhtSensorKind::DHT22
                              : DhtSensorKind::DHT11;
    config_.sample_interval_ms = (raw_config.sample_interval_ms >= kMinSampleIntervalMs)
                                     ? raw_config.sample_interval_ms
                                     : kMinSampleIntervalMs;
    config_.stale_timeout_ms = (raw_config.stale_timeout_ms > 0UL)
                                   ? raw_config.stale_timeout_ms
                                   : kDefaultStaleTimeoutMs;
}

void TempHumiditySensor::invalidateReading(TempHumidityStatus status, DhtReadError error) {
    reading_.status = status;
    reading_.error = error;
    reading_.valid = false;
    reading_.temperature_c = std::numeric_limits<float>::quiet_NaN();
    reading_.humidity_pct = std::numeric_limits<float>::quiet_NaN();
}

bool TempHumiditySensor::isSampleDue(uint32_t now_ms) const {
    if (!has_attempted_sample_) {
        return true;
    }
    return static_cast<uint32_t>(now_ms - last_attempt_ms_) >= config_.sample_interval_ms;
}

bool TempHumiditySensor::refreshStaleness(uint32_t now_ms) {
    if (!has_valid_sample_ || reading_.status != TempHumidityStatus::VALID) {
        return false;
    }
    const uint32_t age_ms = static_cast<uint32_t>(now_ms - last_valid_sample_ms_);
    if (age_ms > config_.stale_timeout_ms) {
        invalidateReading(TempHumidityStatus::STALE, DhtReadError::STALE_TIMEOUT);
        return true;
    }
    return false;
}

DhtPollOutcome TempHumiditySensor::poll(uint32_t now_ms, IDhtRawReader& reader) {
    DhtPollOutcome outcome{};
    const TempHumidityStatus prev_status = reading_.status;

    if (!isSampleDue(now_ms)) {
        const bool stale_triggered = refreshStaleness(now_ms);
        outcome.attempted = false;
        outcome.status_changed = stale_triggered || (reading_.status != prev_status);
        outcome.reading = reading_;
        return outcome;
    }

    has_attempted_sample_ = true;
    last_attempt_ms_ = now_ms;
    outcome.attempted = true;

    float raw_temp_c = std::numeric_limits<float>::quiet_NaN();
    float raw_humidity_pct = std::numeric_limits<float>::quiet_NaN();
    const bool io_ok = reader.readRawSample(&raw_temp_c, &raw_humidity_pct);

    DhtReadError validation_err = DhtReadError::NONE;
    if (!validateRawSample(config_.sensor_kind, raw_temp_c, raw_humidity_pct, io_ok,
                           &validation_err)) {
        ++reading_.consecutive_failures;
        invalidateReading(TempHumidityStatus::READ_ERROR, validation_err);
        outcome.status_changed = (reading_.status != prev_status);
        outcome.reading = reading_;
        return outcome;
    }

    reading_.status = TempHumidityStatus::VALID;
    reading_.error = DhtReadError::NONE;
    reading_.valid = true;
    reading_.temperature_c = raw_temp_c;
    reading_.humidity_pct = raw_humidity_pct;
    reading_.last_sample_ms = now_ms;
    reading_.consecutive_failures = 0UL;
    has_valid_sample_ = true;
    last_valid_sample_ms_ = now_ms;

    outcome.status_changed = (reading_.status != prev_status);
    outcome.reading = reading_;
    return outcome;
}

DhtSensorLimits TempHumiditySensor::sensorLimits(DhtSensorKind kind) {
    if (kind == DhtSensorKind::DHT22) {
        return DhtSensorLimits{-40.0f, 80.0f, 0.0f, 100.0f};
    }
    return DhtSensorLimits{0.0f, 50.0f, 20.0f, 80.0f};
}

bool TempHumiditySensor::validateRawSample(DhtSensorKind kind,
                                           float temperature_c,
                                           float humidity_pct,
                                           bool io_ok,
                                           DhtReadError* out_error) {
    auto set_error = [out_error](DhtReadError err) {
        if (out_error != nullptr) {
            *out_error = err;
        }
    };

    if (!io_ok) {
        set_error(DhtReadError::IO_FAILURE);
        return false;
    }
    if (!std::isfinite(temperature_c) || !std::isfinite(humidity_pct)) {
        set_error(DhtReadError::NON_FINITE_VALUE);
        return false;
    }

    const DhtSensorLimits limits = sensorLimits(kind);
    if (temperature_c < limits.min_temp_c || temperature_c > limits.max_temp_c) {
        set_error(DhtReadError::TEMPERATURE_OUT_OF_RANGE);
        return false;
    }
    if (humidity_pct < limits.min_humidity_pct || humidity_pct > limits.max_humidity_pct) {
        set_error(DhtReadError::HUMIDITY_OUT_OF_RANGE);
        return false;
    }

    set_error(DhtReadError::NONE);
    return true;
}

bool TempHumiditySensor::shouldEmitSerialLog(bool has_logged,
                                             uint32_t now_ms,
                                             uint32_t last_log_ms,
                                             bool status_changed,
                                             uint32_t steady_interval_ms,
                                             uint32_t min_change_interval_ms) {
    if (!has_logged) {
        return true;
    }
    const uint32_t elapsed_ms = static_cast<uint32_t>(now_ms - last_log_ms);
    if (status_changed && elapsed_ms >= min_change_interval_ms) {
        return true;
    }
    return elapsed_ms >= steady_interval_ms;
}

const char* TempHumiditySensor::sensorKindToString(DhtSensorKind kind) {
    switch (kind) {
        case DhtSensorKind::DHT11:
            return "DHT11";
        case DhtSensorKind::DHT22:
            return "DHT22";
        default:
            return "UNKNOWN";
    }
}

const char* TempHumiditySensor::statusToString(TempHumidityStatus status) {
    switch (status) {
        case TempHumidityStatus::UNINITIALIZED:
            return "uninitialized";
        case TempHumidityStatus::VALID:
            return "valid";
        case TempHumidityStatus::READ_ERROR:
            return "read_error";
        case TempHumidityStatus::STALE:
            return "stale";
        default:
            return "unknown";
    }
}

const char* TempHumiditySensor::errorToString(DhtReadError error) {
    switch (error) {
        case DhtReadError::NONE:
            return "none";
        case DhtReadError::IO_FAILURE:
            return "io_failure";
        case DhtReadError::NON_FINITE_VALUE:
            return "non_finite_value";
        case DhtReadError::TEMPERATURE_OUT_OF_RANGE:
            return "temperature_out_of_range";
        case DhtReadError::HUMIDITY_OUT_OF_RANGE:
            return "humidity_out_of_range";
        case DhtReadError::STALE_TIMEOUT:
            return "stale_timeout";
        default:
            return "unknown";
    }
}

}  // namespace node_env
