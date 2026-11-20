#pragma once

#include <cstdint>

namespace node_env {

enum class LightSensorKind : uint8_t {
    BH1750 = 0,
    LDR_ADC_SIM = 1
};

enum class LightSensorStatus : uint8_t {
    UNINITIALIZED = 0,
    VALID = 1,
    READ_ERROR = 2,
    STALE = 3
};

enum class LightReadError : uint8_t {
    NONE = 0,
    NOT_INITIALIZED = 1,
    MEASUREMENT_NOT_READY = 2,
    IO_FAILURE = 3,
    NON_FINITE_VALUE = 4,
    LUX_OUT_OF_RANGE = 5,
    ADC_OUT_OF_RANGE = 6,
    ADC_EXTREME_RAIL = 7,
    STALE_TIMEOUT = 8
};

enum class LightRawStatus : uint8_t {
    OK = 0,
    NOT_INITIALIZED = 1,
    MEASUREMENT_NOT_READY = 2,
    IO_FAILURE = 3,
    ADC_OUT_OF_RANGE = 4,
    ADC_EXTREME_RAIL = 5,
    LUX_OUT_OF_RANGE = 6
};

struct WokwiLdrConversionParams {
    int32_t adc_max = 4095;
    float fixed_resistor_ohm = 10000.0f;
    float rl10_kohm = 50.0f;
    float gamma = 0.7f;
};

struct LightSensorConfig {
    LightSensorKind sensor_kind = LightSensorKind::BH1750;
    uint32_t sample_interval_ms = 1000UL;
    uint32_t stale_timeout_ms = 5000UL;
    float max_valid_lux = 150000.0f;
};

struct LightReading {
    LightSensorKind sensor_kind = LightSensorKind::BH1750;
    LightSensorStatus status = LightSensorStatus::UNINITIALIZED;
    LightReadError error = LightReadError::NONE;
    bool valid = false;
    bool is_simulated_estimate = false;
    float lux = 0.0f;
    uint32_t last_sample_ms = 0UL;
    uint32_t consecutive_failures = 0UL;
};

struct LightPollOutcome {
    bool attempted = false;
    bool status_changed = false;
    LightReading reading{};
};

class ILightRawReader {
public:
    virtual ~ILightRawReader() = default;
    virtual LightRawStatus readRawLux(float* out_lux) = 0;
};

// Pure C++ scheduler, validator, and Wokwi LDR ADC-to-lux converter for Node A.
class LightSensor {
public:
    static constexpr uint32_t kMinSampleIntervalMs = 200UL;
    static constexpr uint32_t kDefaultSampleIntervalMs = 1000UL;
    static constexpr uint32_t kDefaultStaleTimeoutMs = 5000UL;
    static constexpr float kDefaultMaxValidLux = 150000.0f;

    LightSensor();
    explicit LightSensor(const LightSensorConfig& config);

    LightPollOutcome poll(uint32_t now_ms, ILightRawReader& reader);
    bool refreshStaleness(uint32_t now_ms);
    bool isSampleDue(uint32_t now_ms) const;

    const LightSensorConfig& config() const { return config_; }
    const LightReading& reading() const { return reading_; }
    LightSensorStatus status() const { return reading_.status; }

    static bool validateLuxReading(float lux, float max_valid_lux, LightReadError* out_error);
    static LightRawStatus convertWokwiLdrAdcToLux(int32_t raw_adc,
                                                  const WokwiLdrConversionParams& params,
                                                  float max_valid_lux,
                                                  float* out_lux,
                                                  LightReadError* out_error);
    static const char* sensorKindToString(LightSensorKind kind);
    static const char* statusToString(LightSensorStatus status);
    static const char* errorToString(LightReadError error);

private:
    LightSensorConfig config_{};
    LightReading reading_{};
    bool has_attempted_sample_ = false;
    uint32_t last_attempt_ms_ = 0UL;
    bool has_valid_sample_ = false;
    uint32_t last_valid_sample_ms_ = 0UL;

    void sanitizeConfig(const LightSensorConfig& raw_config);
    void invalidateReading(LightSensorStatus status, LightReadError error);
};

}  // namespace node_env
