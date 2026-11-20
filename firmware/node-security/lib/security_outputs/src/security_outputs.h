#pragma once

#include <cstdint>

#include "node_security.h"

namespace node_sec {

struct AlarmOutputPinConfig {
    static constexpr uint8_t kDefaultBuzzerPin = 18U;
    static constexpr uint8_t kDefaultAlarmLedPin = 26U;
    static constexpr uint8_t kInvalidPin = 0xFFU;

    uint8_t buzzer_pin = kDefaultBuzzerPin;
    uint8_t alarm_led_pin = kDefaultAlarmLedPin;
};

struct AlarmOutputPolarityConfig {
    // Default ACTIVE_HIGH assumption: true => HIGH=ON, LOW=OFF (verify on physical module).
    bool buzzer_active_high = true;
    bool alarm_led_active_high = true;
};

struct AlarmOutputConfig {
    AlarmOutputPinConfig pins{};
    AlarmOutputPolarityConfig polarity{};
};

struct AlarmOutputLevels {
    bool buzzer_level_high = false;
    bool alarm_led_level_high = false;
};

struct AlarmOutputSyncOutcome {
    bool initialized = false;
    bool state_changed = false;
    bool outputs_written = false;
    bool alarm_on = false;
    AlarmOutputLevels levels{};
};

// Pure C++ abstraction for digital alarm outputs (GPIO18 buzzer signal, GPIO26 LED).
class IAlarmOutputSink {
public:
    virtual ~IAlarmOutputSink() = default;

    // Configures pin as digital OUTPUT and immediately sets initial_level_high.
    virtual bool configureOutput(uint8_t pin, bool initial_level_high) = 0;

    // Writes digital HIGH (true) or LOW (false) to an already configured output pin.
    virtual bool writeOutput(uint8_t pin, bool level_high) = 0;
};

// Synchronizes Node B buzzer and alarm LED exclusively with SecurityState::alarm_on,
// avoiding redundant GPIO writes when state does not change.
class AlarmOutputSync {
public:
    AlarmOutputSync();
    explicit AlarmOutputSync(const AlarmOutputConfig& config);

    const AlarmOutputConfig& config() const { return config_; }
    bool isInitialized() const { return initialized_; }
    bool lastSyncedAlarmOn() const { return last_alarm_on_; }
    AlarmOutputLevels currentLevels() const { return current_levels_; }

    static bool isValidOutputPin(uint8_t pin);
    static bool isValidConfig(const AlarmOutputConfig& config);
    static AlarmOutputLevels mapAlarmToLevels(bool alarm_on,
                                              const AlarmOutputPolarityConfig& polarity);

    // Configures both outputs to safe OFF state (even when default alarm_on is already false).
    // Calling begin() again resets outputs back to OFF.
    bool begin(IAlarmOutputSink& sink);

    AlarmOutputSyncOutcome sync(IAlarmOutputSink& sink, const SecurityController& controller);
    AlarmOutputSyncOutcome syncState(IAlarmOutputSink& sink, const SecurityState& state);

private:
    AlarmOutputConfig config_{};
    bool initialized_ = false;
    bool last_alarm_on_ = false;
    AlarmOutputLevels current_levels_{};
};

}  // namespace node_sec
