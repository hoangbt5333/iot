#include "light_sensor.h"

#include <cmath>
#include <limits>

namespace node_env {

LightSensor::LightSensor() : LightSensor(LightSensorConfig{}) {}

LightSensor::LightSensor(const LightSensorConfig& config) {
    sanitizeConfig(config);
    reading_.sensor_kind = config_.sensor_kind;
    reading_.is_simulated_estimate = (config_.sensor_kind == LightSensorKind::LDR_ADC_SIM);
    invalidateReading(LightSensorStatus::UNINITIALIZED, LightReadError::NONE);
}

void LightSensor::sanitizeConfig(const LightSensorConfig& raw_config) {
    config_.sensor_kind = (raw_config.sensor_kind == LightSensorKind::LDR_ADC_SIM)
                              ? LightSensorKind::LDR_ADC_SIM
                              : LightSensorKind::BH1750;
    config_.sample_interval_ms = (raw_config.sample_interval_ms >= kMinSampleIntervalMs)
                                     ? raw_config.sample_interval_ms
                                     : kDefaultSampleIntervalMs;
    config_.stale_timeout_ms = (raw_config.stale_timeout_ms > 0UL)
                                   ? raw_config.stale_timeout_ms
                                   : kDefaultStaleTimeoutMs;
    config_.max_valid_lux =
        (std::isfinite(raw_config.max_valid_lux) && raw_config.max_valid_lux > 0.0f)
            ? raw_config.max_valid_lux
            : kDefaultMaxValidLux;
}

void LightSensor::invalidateReading(LightSensorStatus status, LightReadError error) {
    reading_.status = status;
    reading_.error = error;
    reading_.valid = false;
    reading_.lux = std::numeric_limits<float>::quiet_NaN();
}

bool LightSensor::isSampleDue(uint32_t now_ms) const {
    if (!has_attempted_sample_) {
        return true;
    }
    return static_cast<uint32_t>(now_ms - last_attempt_ms_) >= config_.sample_interval_ms;
}

bool LightSensor::refreshStaleness(uint32_t now_ms) {
    if (!has_valid_sample_ || reading_.status != LightSensorStatus::VALID) {
        return false;
    }
    const uint32_t age_ms = static_cast<uint32_t>(now_ms - last_valid_sample_ms_);
    if (age_ms > config_.stale_timeout_ms) {
        invalidateReading(LightSensorStatus::STALE, LightReadError::STALE_TIMEOUT);
        return true;
    }
    return false;
}

LightPollOutcome LightSensor::poll(uint32_t now_ms, ILightRawReader& reader) {
    LightPollOutcome outcome{};
    const LightSensorStatus prev_status = reading_.status;

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

    float raw_lux = std::numeric_limits<float>::quiet_NaN();
    const LightRawStatus raw_status = reader.readRawLux(&raw_lux);

    if (raw_status == LightRawStatus::MEASUREMENT_NOT_READY) {
        if (reading_.status == LightSensorStatus::VALID) {
            refreshStaleness(now_ms);
        } else if (reading_.status == LightSensorStatus::UNINITIALIZED) {
            invalidateReading(LightSensorStatus::UNINITIALIZED, LightReadError::MEASUREMENT_NOT_READY);
        } else {
            invalidateReading(reading_.status, LightReadError::MEASUREMENT_NOT_READY);
        }
        outcome.status_changed = (reading_.status != prev_status);
        outcome.reading = reading_;
        return outcome;
    }

    if (raw_status != LightRawStatus::OK) {
        ++reading_.consecutive_failures;
        LightReadError mapped_err = LightReadError::IO_FAILURE;
        switch (raw_status) {
            case LightRawStatus::NOT_INITIALIZED:
                mapped_err = LightReadError::NOT_INITIALIZED;
                break;
            case LightRawStatus::ADC_OUT_OF_RANGE:
                mapped_err = LightReadError::ADC_OUT_OF_RANGE;
                break;
            case LightRawStatus::ADC_EXTREME_RAIL:
                mapped_err = LightReadError::ADC_EXTREME_RAIL;
                break;
            case LightRawStatus::LUX_OUT_OF_RANGE:
                mapped_err = LightReadError::LUX_OUT_OF_RANGE;
                break;
            case LightRawStatus::IO_FAILURE:
            default:
                mapped_err = LightReadError::IO_FAILURE;
                break;
        }
        invalidateReading(LightSensorStatus::READ_ERROR, mapped_err);
        outcome.status_changed = (reading_.status != prev_status);
        outcome.reading = reading_;
        return outcome;
    }

    LightReadError lux_err = LightReadError::NONE;
    if (!validateLuxReading(raw_lux, config_.max_valid_lux, &lux_err)) {
        ++reading_.consecutive_failures;
        invalidateReading(LightSensorStatus::READ_ERROR, lux_err);
        outcome.status_changed = (reading_.status != prev_status);
        outcome.reading = reading_;
        return outcome;
    }

    reading_.status = LightSensorStatus::VALID;
    reading_.error = LightReadError::NONE;
    reading_.valid = true;
    reading_.lux = raw_lux;
    reading_.last_sample_ms = now_ms;
    reading_.consecutive_failures = 0UL;
    has_valid_sample_ = true;
    last_valid_sample_ms_ = now_ms;

    outcome.status_changed = (reading_.status != prev_status);
    outcome.reading = reading_;
    return outcome;
}

bool LightSensor::validateLuxReading(float lux, float max_valid_lux, LightReadError* out_error) {
    auto set_err = [out_error](LightReadError err) {
        if (out_error != nullptr) {
            *out_error = err;
        }
    };

    if (!std::isfinite(lux)) {
        set_err(LightReadError::NON_FINITE_VALUE);
        return false;
    }
    if (lux < 0.0f || lux > max_valid_lux) {
        set_err(LightReadError::LUX_OUT_OF_RANGE);
        return false;
    }
    set_err(LightReadError::NONE);
    return true;
}

LightRawStatus LightSensor::convertWokwiLdrAdcToLux(int32_t raw_adc,
                                                  const WokwiLdrConversionParams& params,
                                                  float max_valid_lux,
                                                  float* out_lux,
                                                  LightReadError* out_error) {
    auto set_err = [out_error](LightReadError err) {
        if (out_error != nullptr) {
            *out_error = err;
        }
    };

    if (out_lux == nullptr) {
        set_err(LightReadError::IO_FAILURE);
        return LightRawStatus::IO_FAILURE;
    }
    *out_lux = std::numeric_limits<float>::quiet_NaN();

    if (params.adc_max <= 1 || !std::isfinite(params.fixed_resistor_ohm) ||
        params.fixed_resistor_ohm <= 0.0f || !std::isfinite(params.rl10_kohm) ||
        params.rl10_kohm <= 0.0f || !std::isfinite(params.gamma) || params.gamma <= 0.0f ||
        !std::isfinite(max_valid_lux) || max_valid_lux <= 0.0f) {
        set_err(LightReadError::IO_FAILURE);
        return LightRawStatus::IO_FAILURE;
    }

    if (raw_adc < 0 || raw_adc > params.adc_max) {
        set_err(LightReadError::ADC_OUT_OF_RANGE);
        return LightRawStatus::ADC_OUT_OF_RANGE;
    }

    // Rail extremes 0 and adc_max cause division by zero and cannot distinguish faults from saturation.
    if (raw_adc == 0 || raw_adc == params.adc_max) {
        set_err(LightReadError::ADC_EXTREME_RAIL);
        return LightRawStatus::ADC_EXTREME_RAIL;
    }

    const double adc_d = static_cast<double>(raw_adc);
    const double adc_max_d = static_cast<double>(params.adc_max);
    const double denom = adc_max_d - adc_d;
    if (denom <= 0.0) {
        set_err(LightReadError::ADC_EXTREME_RAIL);
        return LightRawStatus::ADC_EXTREME_RAIL;
    }

    // Wokwi photoresistor divider: R_ldr = R_fixed * V_AO / (VCC - V_AO) = R_fixed * adc / (adc_max - adc).
    const double r_ldr_ohm = static_cast<double>(params.fixed_resistor_ohm) * (adc_d / denom);
    if (!std::isfinite(r_ldr_ohm) || r_ldr_ohm <= 0.0) {
        set_err(LightReadError::ADC_EXTREME_RAIL);
        return LightRawStatus::ADC_EXTREME_RAIL;
    }

    const double rl10_ohm = static_cast<double>(params.rl10_kohm) * 1000.0;
    const double base = (rl10_ohm * std::pow(10.0, static_cast<double>(params.gamma))) / r_ldr_ohm;
    const double lux_d = std::pow(base, 1.0 / static_cast<double>(params.gamma));
    if (!std::isfinite(lux_d) || lux_d < 0.0 || lux_d > static_cast<double>(max_valid_lux)) {
        set_err(LightReadError::LUX_OUT_OF_RANGE);
        return LightRawStatus::LUX_OUT_OF_RANGE;
    }

    *out_lux = static_cast<float>(lux_d);
    set_err(LightReadError::NONE);
    return LightRawStatus::OK;
}

const char* LightSensor::sensorKindToString(LightSensorKind kind) {
    switch (kind) {
        case LightSensorKind::BH1750:
            return "BH1750";
        case LightSensorKind::LDR_ADC_SIM:
            return "LDR_SIM";
        default:
            return "UNKNOWN";
    }
}

const char* LightSensor::statusToString(LightSensorStatus status) {
    switch (status) {
        case LightSensorStatus::UNINITIALIZED:
            return "uninitialized";
        case LightSensorStatus::VALID:
            return "valid";
        case LightSensorStatus::READ_ERROR:
            return "read_error";
        case LightSensorStatus::STALE:
            return "stale";
        default:
            return "unknown";
    }
}

const char* LightSensor::errorToString(LightReadError error) {
    switch (error) {
        case LightReadError::NONE:
            return "none";
        case LightReadError::NOT_INITIALIZED:
            return "not_initialized";
        case LightReadError::MEASUREMENT_NOT_READY:
            return "measurement_not_ready";
        case LightReadError::IO_FAILURE:
            return "io_failure";
        case LightReadError::NON_FINITE_VALUE:
            return "non_finite_value";
        case LightReadError::LUX_OUT_OF_RANGE:
            return "lux_out_of_range";
        case LightReadError::ADC_OUT_OF_RANGE:
            return "adc_out_of_range";
        case LightReadError::ADC_EXTREME_RAIL:
            return "adc_extreme_rail";
        case LightReadError::STALE_TIMEOUT:
            return "stale_timeout";
        default:
            return "unknown";
    }
}

}  // namespace node_env
