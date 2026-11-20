#include "security_outputs.h"

namespace node_sec {

AlarmOutputSync::AlarmOutputSync() : AlarmOutputSync(AlarmOutputConfig{}) {}

AlarmOutputSync::AlarmOutputSync(const AlarmOutputConfig& config) : config_(config) {
    current_levels_ = mapAlarmToLevels(false, config_.polarity);
}

bool AlarmOutputSync::isValidOutputPin(uint8_t pin) {
    if (pin == AlarmOutputPinConfig::kInvalidPin) {
        return false;
    }
    // ESP32 GPIO6..11 are reserved for SPI flash.
    if (pin >= 6U && pin <= 11U) {
        return false;
    }
    // ESP32 GPIO34..39 are input-only without output drivers.
    if (pin >= 34U && pin <= 39U) {
        return false;
    }
    return true;
}

bool AlarmOutputSync::isValidConfig(const AlarmOutputConfig& config) {
    if (!isValidOutputPin(config.pins.buzzer_pin) ||
        !isValidOutputPin(config.pins.alarm_led_pin)) {
        return false;
    }
    if (config.pins.buzzer_pin == config.pins.alarm_led_pin) {
        return false;
    }
    return true;
}

AlarmOutputLevels AlarmOutputSync::mapAlarmToLevels(bool alarm_on,
                                                    const AlarmOutputPolarityConfig& polarity) {
    AlarmOutputLevels levels;
    levels.buzzer_level_high = (alarm_on == polarity.buzzer_active_high);
    levels.alarm_led_level_high = (alarm_on == polarity.alarm_led_active_high);
    return levels;
}

bool AlarmOutputSync::begin(IAlarmOutputSink& sink) {
    if (!isValidConfig(config_)) {
        initialized_ = false;
        return false;
    }

    const AlarmOutputLevels off_levels = mapAlarmToLevels(false, config_.polarity);
    const bool buzzer_ok = sink.configureOutput(config_.pins.buzzer_pin,
                                                off_levels.buzzer_level_high);
    const bool led_ok = sink.configureOutput(config_.pins.alarm_led_pin,
                                             off_levels.alarm_led_level_high);
    if (!buzzer_ok || !led_ok) {
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    last_alarm_on_ = false;
    current_levels_ = off_levels;
    return true;
}

AlarmOutputSyncOutcome AlarmOutputSync::sync(IAlarmOutputSink& sink,
                                             const SecurityController& controller) {
    return syncState(sink, controller.state());
}

AlarmOutputSyncOutcome AlarmOutputSync::syncState(IAlarmOutputSink& sink,
                                                  const SecurityState& state) {
    AlarmOutputSyncOutcome outcome;
    outcome.initialized = initialized_;
    outcome.alarm_on = last_alarm_on_;
    outcome.levels = current_levels_;

    if (!initialized_) {
        return outcome;
    }

    if (state.alarm_on == last_alarm_on_) {
        return outcome;
    }

    const AlarmOutputLevels target_levels = mapAlarmToLevels(state.alarm_on, config_.polarity);
    const bool buzzer_ok =
        sink.writeOutput(config_.pins.buzzer_pin, target_levels.buzzer_level_high);
    const bool led_ok =
        sink.writeOutput(config_.pins.alarm_led_pin, target_levels.alarm_led_level_high);

    if (!buzzer_ok || !led_ok) {
        return outcome;
    }

    last_alarm_on_ = state.alarm_on;
    current_levels_ = target_levels;
    outcome.state_changed = true;
    outcome.outputs_written = true;
    outcome.alarm_on = last_alarm_on_;
    outcome.levels = current_levels_;
    return outcome;
}

}  // namespace node_sec
