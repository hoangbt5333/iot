#include "node_automation.h"

#include <cmath>
#include <cstring>

namespace node_env {

AutomationController::AutomationController() = default;

AutomationController::AutomationController(const AutomationThresholds& initial_thresholds) {
    if (isValidThresholds(initial_thresholds)) {
        state_.thresholds = initial_thresholds;
    }
}

AutomationState AutomationController::evaluateSensors(const SensorReading& reading) {
    const bool temp_ok = isValidTemperatureReading(reading.temperature_c, reading.temperature_valid);
    const bool light_ok = isValidLuxReading(reading.light_lux, reading.light_lux_valid);

    updateSensorHealth(temp_ok, light_ok);

    if (state_.mode == DeviceMode::AUTO) {
        if (light_ok) {
            evaluateLampAuto(reading.light_lux);
        }
        if (temp_ok) {
            evaluateFanAuto(reading.temperature_c);
        }
    }

    return state_;
}

CommandResult AutomationController::setMode(DeviceMode new_mode) {
    if (new_mode != DeviceMode::AUTO && new_mode != DeviceMode::MANUAL) {
        return makeRejected(CommandRejectReason::INVALID_MODE);
    }
    state_.mode = new_mode;
    return makeApplied();
}

CommandResult AutomationController::setModeFromString(const char* mode_str) {
    DeviceMode parsed = DeviceMode::AUTO;
    if (!parseMode(mode_str, &parsed)) {
        return makeRejected(CommandRejectReason::INVALID_MODE);
    }
    return setMode(parsed);
}

CommandResult AutomationController::setLampManual(bool on) {
    state_.mode = DeviceMode::MANUAL;
    state_.lamp_on = on;
    return makeApplied();
}

CommandResult AutomationController::setFanManual(bool on) {
    state_.mode = DeviceMode::MANUAL;
    state_.fan_on = on;
    return makeApplied();
}

CommandResult AutomationController::setThresholds(const AutomationThresholds& thresholds) {
    if (!isValidThresholds(thresholds)) {
        return makeRejected(CommandRejectReason::INVALID_THRESHOLDS);
    }
    state_.thresholds = thresholds;
    return makeApplied();
}

CommandResult AutomationController::applyCommand(const NodeACommand& cmd) {
    switch (cmd.target) {
        case CommandTarget::LAMP:
        case CommandTarget::FAN: {
            if (cmd.action != CommandAction::SET) {
                return makeRejected(CommandRejectReason::INVALID_ACTION);
            }
            if (cmd.mode_value_invalid) {
                return makeRejected(CommandRejectReason::INVALID_MODE);
            }
            if (cmd.has_mode_value) {
                if (cmd.mode_value == DeviceMode::AUTO) {
                    return makeRejected(CommandRejectReason::CONTRADICTORY_MODE);
                }
                if (cmd.mode_value != DeviceMode::MANUAL) {
                    return makeRejected(CommandRejectReason::INVALID_MODE);
                }
            }
            if (!cmd.has_bool_value) {
                return makeRejected(CommandRejectReason::INVALID_VALUE);
            }
            return (cmd.target == CommandTarget::LAMP) ? setLampManual(cmd.bool_value)
                                                       : setFanManual(cmd.bool_value);
        }
        case CommandTarget::DEVICE_MODE: {
            if (cmd.action != CommandAction::SET) {
                return makeRejected(CommandRejectReason::INVALID_ACTION);
            }
            if (cmd.mode_value_invalid || !cmd.has_mode_value) {
                return makeRejected(CommandRejectReason::INVALID_MODE);
            }
            return setMode(cmd.mode_value);
        }
        case CommandTarget::THRESHOLDS: {
            if (cmd.action != CommandAction::UPDATE) {
                return makeRejected(CommandRejectReason::INVALID_ACTION);
            }
            if (!cmd.has_thresholds_value) {
                return makeRejected(CommandRejectReason::INVALID_THRESHOLDS);
            }
            return setThresholds(cmd.thresholds_value);
        }
        case CommandTarget::UNKNOWN:
        default:
            return makeRejected(CommandRejectReason::INVALID_TARGET);
    }
}

bool AutomationController::isValidThresholds(const AutomationThresholds& thresholds) {
    if (!std::isfinite(thresholds.light_on_lux) || !std::isfinite(thresholds.light_off_lux) ||
        !std::isfinite(thresholds.fan_on_temp_c) || !std::isfinite(thresholds.fan_off_temp_c)) {
        return false;
    }
    if (thresholds.light_on_lux < kMinValidLux ||
        thresholds.light_on_lux >= thresholds.light_off_lux) {
        return false;
    }
    if (thresholds.fan_off_temp_c < kMinValidTempC ||
        thresholds.fan_on_temp_c > kMaxValidTempC ||
        thresholds.fan_off_temp_c >= thresholds.fan_on_temp_c) {
        return false;
    }
    return true;
}

bool AutomationController::isValidTemperatureReading(float temp_c, bool valid_flag) {
    return valid_flag && std::isfinite(temp_c) && temp_c >= kMinValidTempC && temp_c <= kMaxValidTempC;
}

bool AutomationController::isValidLuxReading(float lux, bool valid_flag) {
    return valid_flag && std::isfinite(lux) && lux >= kMinValidLux;
}

bool AutomationController::parseMode(const char* mode_str, DeviceMode* out_mode) {
    if (mode_str == nullptr || out_mode == nullptr) {
        return false;
    }
    if (std::strcmp(mode_str, "AUTO") == 0) {
        *out_mode = DeviceMode::AUTO;
        return true;
    }
    if (std::strcmp(mode_str, "MANUAL") == 0) {
        *out_mode = DeviceMode::MANUAL;
        return true;
    }
    return false;
}

const char* AutomationController::modeToString(DeviceMode mode) {
    switch (mode) {
        case DeviceMode::AUTO:
            return "AUTO";
        case DeviceMode::MANUAL:
            return "MANUAL";
        default:
            return "UNKNOWN";
    }
}

const char* AutomationController::healthToString(SensorHealth health) {
    switch (health) {
        case SensorHealth::UNINITIALIZED:
            return "uninitialized";
        case SensorHealth::OK:
            return "ok";
        case SensorHealth::DEGRADED:
            return "degraded";
        case SensorHealth::FAULT:
        default:
            return "fault";
    }
}

const char* AutomationController::rejectReasonToString(CommandRejectReason reason) {
    switch (reason) {
        case CommandRejectReason::NONE:
            return "none";
        case CommandRejectReason::INVALID_TARGET:
            return "invalid_target";
        case CommandRejectReason::INVALID_ACTION:
            return "invalid_action";
        case CommandRejectReason::INVALID_MODE:
            return "invalid_mode";
        case CommandRejectReason::INVALID_VALUE:
            return "invalid_value";
        case CommandRejectReason::CONTRADICTORY_MODE:
            return "contradictory_mode";
        case CommandRejectReason::INVALID_THRESHOLDS:
            return "invalid_thresholds";
        default:
            return "unknown";
    }
}

void AutomationController::updateSensorHealth(bool temp_ok, bool light_ok) {
    if (temp_ok && light_ok) {
        state_.sensor_health = SensorHealth::OK;
    } else if (temp_ok || light_ok) {
        state_.sensor_health = SensorHealth::DEGRADED;
    } else {
        state_.sensor_health = SensorHealth::FAULT;
    }
}

void AutomationController::evaluateLampAuto(float lux) {
    if (lux < state_.thresholds.light_on_lux) {
        state_.lamp_on = true;
    } else if (lux > state_.thresholds.light_off_lux) {
        state_.lamp_on = false;
    }
}

void AutomationController::evaluateFanAuto(float temp_c) {
    if (temp_c >= state_.thresholds.fan_on_temp_c) {
        state_.fan_on = true;
    } else if (temp_c < state_.thresholds.fan_off_temp_c) {
        state_.fan_on = false;
    }
}

CommandResult AutomationController::makeApplied() const {
    CommandResult res;
    res.status = CommandStatus::APPLIED;
    res.reason = CommandRejectReason::NONE;
    res.state = state_;
    return res;
}

CommandResult AutomationController::makeRejected(CommandRejectReason reason) const {
    CommandResult res;
    res.status = CommandStatus::REJECTED;
    res.reason = reason;
    res.state = state_;
    return res;
}

}  // namespace node_env
