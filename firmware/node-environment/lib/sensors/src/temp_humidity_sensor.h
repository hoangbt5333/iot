#pragma once

#include <cstdint>

namespace node_env {

enum class DhtSensorKind : uint8_t {
    DHT11 = 0,
    DHT22 = 1
};

enum class TempHumidityStatus : uint8_t {
    UNINITIALIZED = 0,
    VALID = 1,
    READ_ERROR = 2,
    STALE = 3
};

enum class DhtReadError : uint8_t {
    NONE = 0,
    IO_FAILURE = 1,
    NON_FINITE_VALUE = 2,
    TEMPERATURE_OUT_OF_RANGE = 3,
    HUMIDITY_OUT_OF_RANGE = 4,
    STALE_TIMEOUT = 5
};

struct DhtSensorLimits {
    float min_temp_c = 0.0f;
    float max_temp_c = 50.0f;
    float min_humidity_pct = 20.0f;
    float max_humidity_pct = 80.0f;
};

struct TempHumidityConfig {
    DhtSensorKind sensor_kind = DhtSensorKind::DHT11;
    uint32_t sample_interval_ms = 2000UL;
    uint32_t stale_timeout_ms = 6000UL;
};

struct TempHumidityReading {
    DhtSensorKind sensor_kind = DhtSensorKind::DHT11;
    TempHumidityStatus status = TempHumidityStatus::UNINITIALIZED;
    DhtReadError error = DhtReadError::NONE;
    bool valid = false;
    float temperature_c = 0.0f;
    float humidity_pct = 0.0f;
    uint32_t last_sample_ms = 0UL;
    uint32_t consecutive_failures = 0UL;
};

struct DhtPollOutcome {
    bool attempted = false;
    bool status_changed = false;
    TempHumidityReading reading{};
};

class IDhtRawReader {
public:
    virtual ~IDhtRawReader() = default;
    virtual bool readRawSample(float* out_temp_c, float* out_humidity_pct) = 0;
};

// Pure C++ scheduler and validator for Node A DHT11 (REAL) / DHT22 (SIM) sensors.
class TempHumiditySensor {
public:
    static constexpr uint32_t kMinSampleIntervalMs = 2000UL;
    static constexpr uint32_t kDefaultStaleTimeoutMs = 6000UL;

    TempHumiditySensor();
    explicit TempHumiditySensor(const TempHumidityConfig& config);

    // Polls reader on boot and whenever uint32_t elapsed >= sample_interval_ms; tracks staleness otherwise.
    DhtPollOutcome poll(uint32_t now_ms, IDhtRawReader& reader);

    // Transitions VALID -> STALE if age exceeds stale_timeout_ms without a newer valid sample.
    bool refreshStaleness(uint32_t now_ms);

    bool isSampleDue(uint32_t now_ms) const;
    const TempHumidityConfig& config() const { return config_; }
    const TempHumidityReading& reading() const { return reading_; }
    TempHumidityStatus status() const { return reading_.status; }

    static DhtSensorLimits sensorLimits(DhtSensorKind kind);
    static bool validateRawSample(DhtSensorKind kind,
                                  float temperature_c,
                                  float humidity_pct,
                                  bool io_ok,
                                  DhtReadError* out_error);
    static bool shouldEmitSerialLog(bool has_logged,
                                    uint32_t now_ms,
                                    uint32_t last_log_ms,
                                    bool status_changed,
                                    uint32_t steady_interval_ms,
                                    uint32_t min_change_interval_ms = kMinSampleIntervalMs);
    static const char* sensorKindToString(DhtSensorKind kind);
    static const char* statusToString(TempHumidityStatus status);
    static const char* errorToString(DhtReadError error);

private:
    TempHumidityConfig config_{};
    TempHumidityReading reading_{};
    bool has_attempted_sample_ = false;
    uint32_t last_attempt_ms_ = 0UL;
    bool has_valid_sample_ = false;
    uint32_t last_valid_sample_ms_ = 0UL;

    void sanitizeConfig(const TempHumidityConfig& raw_config);
    void invalidateReading(TempHumidityStatus status, DhtReadError error);
};

}  // namespace node_env
