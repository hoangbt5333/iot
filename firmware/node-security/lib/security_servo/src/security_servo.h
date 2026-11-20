#pragma once

#include <cstdint>

#include "node_security.h"

namespace node_sec {

enum class ServoSyncError : uint8_t {
    NONE = 0,
    NOT_INITIALIZED = 1,
    INVALID_CONFIG = 2,
    ANGLE_OUT_OF_SAFE_RANGE = 3,
    ATTACH_FAILED = 4,
    WRITE_FAILED = 5,
    FAULT_LATCHED_RETRY_LIMIT = 6
};

struct ServoOutputConfig {
    static constexpr uint8_t kDefaultServoPin = 19U;
    static constexpr uint8_t kInvalidPin = 0xFFU;
    static constexpr uint16_t kDefaultPwmFreqHz = 50U;
    static constexpr uint16_t kDefaultMinPulseUs = 500U;
    static constexpr uint16_t kDefaultMaxPulseUs = 2400U;
    static constexpr uint16_t kMinSupportedPulseUs = 500U;
    static constexpr uint16_t kMaxSupportedPulseUs = 2500U;
    static constexpr uint8_t kDefaultMaxConsecutiveFailures = 3U;

    uint8_t servo_pin = kDefaultServoPin;
    uint16_t pwm_freq_hz = kDefaultPwmFreqHz;
    uint16_t min_pulse_us = kDefaultMinPulseUs;
    uint16_t max_pulse_us = kDefaultMaxPulseUs;
    ServoSafetyConfig safety{};
    uint8_t max_consecutive_failures = kDefaultMaxConsecutiveFailures;
};

struct ServoOutputSyncOutcome {
    bool initialized = false;
    bool attached = false;
    bool move_attempted = false;
    bool applied = false;
    bool fault_latched = false;
    bool retry_suppressed = false;
    ServoSyncError error = ServoSyncError::NONE;
    int16_t target_angle_deg = 0;
    int16_t applied_angle_deg = 0;
    uint8_t consecutive_failures = 0U;
};

// Pure C++ abstraction for ESP32 PWM servo sink (decoupled from ESP32Servo/Arduino).
class IServoOutputSink {
public:
    virtual ~IServoOutputSink() = default;

    virtual bool isAttached() const = 0;
    virtual bool attach(uint8_t pin,
                        uint16_t pwm_freq_hz,
                        uint16_t min_pulse_us,
                        uint16_t max_pulse_us) = 0;
    virtual bool writeAngle(int16_t angle_deg) = 0;
    virtual void detach() = 0;
};

// Synchronizes Node B SG90 door-model servo on GPIO19 ONLY when SecurityState::servo_move_requested
// is true and target angle lies within configured safety bounds.
// Delays attach() until first valid request to avoid default-pulse movement on boot.
class ServoOutputSync {
public:
    ServoOutputSync();
    explicit ServoOutputSync(const ServoOutputConfig& config);

    const ServoOutputConfig& config() const { return config_; }
    bool isInitialized() const { return initialized_; }
    bool hasAppliedMove() const { return has_applied_move_; }
    int16_t lastAppliedAngleDeg() const { return last_applied_angle_deg_; }
    bool isFaultLatched() const { return fault_latched_; }
    uint8_t consecutiveFailures() const { return consecutive_failures_; }

    static bool isValidServoPwmPin(uint8_t pin);
    static bool isValidConfig(const ServoOutputConfig& config);
    static const char* errorToString(ServoSyncError error);

    // Validates configuration without attaching PWM or moving servo at boot.
    bool begin();

    // Clears latched retry-limit fault so subsequent sync attempts can recover.
    void clearFault();

    ServoOutputSyncOutcome sync(IServoOutputSink& sink, const SecurityController& controller);
    ServoOutputSyncOutcome syncState(IServoOutputSink& sink,
                                     const SecurityState& state,
                                     const ServoSafetyConfig* active_safety = nullptr);

private:
    ServoOutputConfig config_{};
    bool initialized_ = false;
    bool has_applied_move_ = false;
    int16_t last_applied_angle_deg_ = 0;
    bool fault_latched_ = false;
    int16_t failed_target_angle_deg_ = 0;
    uint8_t consecutive_failures_ = 0U;

    ServoOutputSyncOutcome recordFailure(IServoOutputSink& sink,
                                         ServoSyncError error,
                                         int16_t target_angle_deg);
};

}  // namespace node_sec
