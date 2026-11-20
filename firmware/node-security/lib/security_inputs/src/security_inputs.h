#pragma once

#include <cstdint>

#include "node_security.h"

namespace node_sec {

struct SecurityInputPinConfig {
    static constexpr uint8_t kDefaultPirPin = 27U;
    static constexpr uint8_t kDefaultReedPin = 32U;
    static constexpr uint8_t kInvalidPin = 0xFFU;

    uint8_t pir_pin = kDefaultPirPin;
    uint8_t reed_pin = kDefaultReedPin;
    bool reed_use_internal_pullup = true;
};

struct SecurityInputPolarityConfig {
    // Digital HIGH from PIR HC-SR501 / SIM PIR means motion detected.
    bool pir_motion_active_high = true;
    // Proposed wiring: reed switch between GPIO32 and GND with INPUT_PULLUP:
    // CLOSED => LOW (door_open=false), OPEN => HIGH (door_open=true).
    // Override here if physical reed module uses inverted polarity.
    bool reed_door_open_high = true;
};

struct SecurityInputSamplerConfig {
    static constexpr uint32_t kDefaultSampleIntervalMs = 10UL;

    SecurityInputPolarityConfig polarity{};
    uint32_t sample_interval_ms = kDefaultSampleIntervalMs;
};

struct RawSecurityInputSample {
    bool valid = false;
    bool pir_level_high = false;
    bool reed_level_high = false;
};

struct MappedSecurityInput {
    bool valid = false;
    bool pir_motion_detected = false;
    bool door_raw_open = false;
};

struct SecuritySamplerStep {
    bool polled = false;
    bool sample_valid = false;
    MappedSecurityInput mapped{};
    StepOutcome door_outcome{};
    StepOutcome motion_outcome{};
    bool any_alarm_triggered = false;
    SecurityState state{};
};

// Pure C++ abstraction for Node B digital security inputs (PIR + reed switch).
class ISecurityInputSource {
public:
    virtual ~ISecurityInputSource() = default;

    // Configures input hardware/source before first read; returns false if setup fails.
    virtual bool begin() = 0;

    // Reads current digital pin states; returns valid=false until source is initialized/valid.
    virtual RawSecurityInputSample readInputs() = 0;
};

// Maps raw digital logic levels using centralized polarity and feeds SecurityController
// on a millis() rollover-safe schedule without duplicating debounce state machines.
class SecurityInputSampler {
public:
    SecurityInputSampler();
    explicit SecurityInputSampler(const SecurityInputSamplerConfig& config);

    const SecurityInputSamplerConfig& config() const { return config_; }
    bool hasPolled() const { return has_polled_; }
    bool hasValidSample() const { return has_valid_sample_; }

    void reset();

    static MappedSecurityInput mapRawSample(const RawSecurityInputSample& raw,
                                            const SecurityInputPolarityConfig& polarity);

    SecuritySamplerStep poll(ISecurityInputSource& source,
                             SecurityController& controller,
                             uint32_t now_ms);

private:
    SecurityInputSamplerConfig config_{};
    bool has_polled_ = false;
    bool has_valid_sample_ = false;
    uint32_t last_poll_ms_ = 0UL;
};

}  // namespace node_sec
