#include "node_security.h"

#include <cstring>

namespace node_sec {

DebouncedInput::DebouncedInput(bool initial_stable, uint32_t debounce_ms)
    : stable_state_(initial_stable),
      last_raw_state_(initial_stable),
      pending_change_(false),
      last_transition_ms_(0),
      debounce_ms_(debounce_ms) {}

bool DebouncedInput::update(bool raw_state, uint32_t now_ms) {
    if (raw_state != last_raw_state_) {
        last_raw_state_ = raw_state;
        last_transition_ms_ = now_ms;
        pending_change_ = (raw_state != stable_state_);
    }

    if (pending_change_ &&
        (static_cast<uint32_t>(now_ms - last_transition_ms_) >= debounce_ms_)) {
        stable_state_ = last_raw_state_;
        pending_change_ = false;
        return true;
    }

    return false;
}

void DebouncedInput::reset(bool stable_state, uint32_t now_ms) {
    stable_state_ = stable_state;
    last_raw_state_ = stable_state;
    pending_change_ = false;
    last_transition_ms_ = now_ms;
}

SecurityController::SecurityController() : SecurityController(SecurityPolicyConfig{}) {}

SecurityController::SecurityController(const SecurityPolicyConfig& config)
    : door_debouncer_(false, config.door_debounce_ms),
      switch_debouncer_(false, config.switch_debounce_ms) {
    sanitizeConfig(config);
    state_.mode = SecurityMode::HOME;
    state_.door_open = false;
    state_.motion_detected = false;
    state_.alarm_on = false;
    state_.servo_angle = config_.servo.initial_angle_deg;
    state_.servo_move_requested = false;
}

void SecurityController::sanitizeConfig(const SecurityPolicyConfig& raw_config) {
    config_.pir_triggers_alarm_in_away = raw_config.pir_triggers_alarm_in_away;
    config_.switch_arbitration = raw_config.switch_arbitration;
    config_.door_debounce_ms = raw_config.door_debounce_ms;
    config_.switch_debounce_ms = raw_config.switch_debounce_ms;

    if (isValidServoConfig(raw_config.servo)) {
        config_.servo = raw_config.servo;
    } else {
        config_.servo = ServoSafetyConfig{};
    }
}

StepOutcome SecurityController::updateDoorInput(bool raw_door_open, uint32_t now_ms) {
    StepOutcome outcome = makeOutcome(TriggerSource::REED_SWITCH);
    const bool changed = door_debouncer_.update(raw_door_open, now_ms);
    if (!changed) {
        return outcome;
    }

    state_.door_open = door_debouncer_.stableState();
    outcome.door_changed = true;

    // Local edge rule: door open in AWAY triggers alarm even when MQTT is offline.
    // Closing the door does NOT silence an already latched alarm.
    if (state_.mode == SecurityMode::AWAY && state_.door_open && !state_.alarm_on) {
        state_.alarm_on = true;
        outcome.alarm_triggered = true;
        outcome.trigger_source = TriggerSource::EDGE_RULE;
    }

    outcome.state = state_;
    return outcome;
}

StepOutcome SecurityController::updateMotionInput(bool pir_high) {
    StepOutcome outcome = makeOutcome(TriggerSource::PIR);
    const bool previous = state_.motion_detected;
    state_.motion_detected = pir_high;

    if (!previous && pir_high) {
        outcome.motion_rising_edge = true;
        if (config_.pir_triggers_alarm_in_away && state_.mode == SecurityMode::AWAY &&
            !state_.alarm_on) {
            state_.alarm_on = true;
            outcome.alarm_triggered = true;
            outcome.trigger_source = TriggerSource::EDGE_RULE;
        }
    }

    outcome.state = state_;
    return outcome;
}

StepOutcome SecurityController::updatePhysicalModeSwitch(bool raw_switch_away, uint32_t now_ms) {
    StepOutcome outcome = makeOutcome(TriggerSource::PHYSICAL_SWITCH);
    const bool edge_changed = switch_debouncer_.update(raw_switch_away, now_ms);

    if (config_.switch_arbitration == SwitchArbitrationMode::DISABLED_PENDING_APPROVAL) {
        return outcome;
    }

    const SecurityMode target_mode =
        switch_debouncer_.stableState() ? SecurityMode::AWAY : SecurityMode::HOME;

    if (config_.switch_arbitration == SwitchArbitrationMode::EDGE_TRANSITION_LAST_WRITER_WINS) {
        if (!edge_changed) {
            return outcome;
        }
        return setMode(target_mode, TriggerSource::PHYSICAL_SWITCH).outcome;
    }

    if (config_.switch_arbitration == SwitchArbitrationMode::PHYSICAL_LEVEL_OVERRIDES) {
        if (target_mode == state_.mode) {
            return outcome;
        }
        return setMode(target_mode, TriggerSource::PHYSICAL_SWITCH).outcome;
    }

    return outcome;
}

CommandResult SecurityController::setMode(SecurityMode new_mode, TriggerSource source) {
    if (!isValidMode(new_mode)) {
        return makeRejected(CommandRejectReason::INVALID_MODE);
    }

    StepOutcome outcome = makeOutcome(source);
    if (state_.mode != new_mode) {
        state_.mode = new_mode;
        outcome.mode_changed = true;
    }

    if (new_mode == SecurityMode::HOME) {
        if (state_.alarm_on) {
            state_.alarm_on = false;
            outcome.alarm_cleared = true;
        }
    } else if (new_mode == SecurityMode::AWAY) {
        if (state_.door_open && !state_.alarm_on) {
            state_.alarm_on = true;
            outcome.alarm_triggered = true;
        }
    }

    outcome.state = state_;
    return makeApplied(outcome);
}

CommandResult SecurityController::setModeFromString(const char* mode_str, TriggerSource source) {
    SecurityMode parsed = SecurityMode::HOME;
    if (!parseMode(mode_str, &parsed)) {
        return makeRejected(CommandRejectReason::INVALID_MODE);
    }
    return setMode(parsed, source);
}

CommandResult SecurityController::setServoAngle(int32_t angle_deg) {
    if (angle_deg < static_cast<int32_t>(config_.servo.min_angle_deg) ||
        angle_deg > static_cast<int32_t>(config_.servo.max_angle_deg)) {
        return makeRejected(CommandRejectReason::SERVO_ANGLE_OUT_OF_SAFE_RANGE);
    }

    state_.servo_angle = static_cast<int16_t>(angle_deg);
    state_.servo_move_requested = true;

    StepOutcome outcome = makeOutcome(TriggerSource::MQTT_COMMAND);
    outcome.servo_moved = true;
    outcome.state = state_;
    return makeApplied(outcome);
}

CommandResult SecurityController::setAlarm(bool enable, TriggerSource source) {
    StepOutcome outcome = makeOutcome(source);
    if (enable && !state_.alarm_on) {
        state_.alarm_on = true;
        outcome.alarm_triggered = true;
    } else if (!enable && state_.alarm_on) {
        state_.alarm_on = false;
        outcome.alarm_cleared = true;
    }
    outcome.state = state_;
    return makeApplied(outcome);
}

CommandResult SecurityController::applyCommand(const NodeBCommand& cmd) {
    switch (cmd.target) {
        case CommandTarget::SECURITY_MODE: {
            if (cmd.action != CommandAction::SET) {
                return makeRejected(CommandRejectReason::INVALID_ACTION);
            }
            if (cmd.mode_value_invalid || !cmd.has_mode_value) {
                return makeRejected(CommandRejectReason::INVALID_MODE);
            }
            return setMode(cmd.mode_value, TriggerSource::MQTT_COMMAND);
        }
        case CommandTarget::SERVO: {
            if (cmd.action != CommandAction::SET) {
                return makeRejected(CommandRejectReason::INVALID_ACTION);
            }
            if (!cmd.has_int_value) {
                return makeRejected(CommandRejectReason::INVALID_VALUE);
            }
            return setServoAngle(cmd.int_value);
        }
        case CommandTarget::ALARM: {
            if (cmd.action == CommandAction::SILENCE) {
                return setAlarm(false, TriggerSource::MQTT_COMMAND);
            }
            if (cmd.action == CommandAction::SET) {
                if (!cmd.has_bool_value) {
                    return makeRejected(CommandRejectReason::INVALID_VALUE);
                }
                return setAlarm(cmd.bool_value, TriggerSource::MQTT_COMMAND);
            }
            return makeRejected(CommandRejectReason::INVALID_ACTION);
        }
        case CommandTarget::UNKNOWN:
        default:
            return makeRejected(CommandRejectReason::INVALID_TARGET);
    }
}

bool SecurityController::isValidServoConfig(const ServoSafetyConfig& servo) {
    if (servo.min_angle_deg < kAbsoluteMinServoDeg ||
        servo.max_angle_deg > kAbsoluteMaxServoDeg ||
        servo.min_angle_deg > servo.max_angle_deg) {
        return false;
    }
    if (servo.initial_angle_deg < servo.min_angle_deg ||
        servo.initial_angle_deg > servo.max_angle_deg) {
        return false;
    }
    return true;
}

bool SecurityController::isValidMode(SecurityMode mode) {
    return mode == SecurityMode::HOME || mode == SecurityMode::AWAY;
}

bool SecurityController::parseMode(const char* mode_str, SecurityMode* out_mode) {
    if (mode_str == nullptr || out_mode == nullptr) {
        return false;
    }
    if (std::strcmp(mode_str, "HOME") == 0) {
        *out_mode = SecurityMode::HOME;
        return true;
    }
    if (std::strcmp(mode_str, "AWAY") == 0) {
        *out_mode = SecurityMode::AWAY;
        return true;
    }
    return false;
}

const char* SecurityController::modeToString(SecurityMode mode) {
    switch (mode) {
        case SecurityMode::HOME:
            return "HOME";
        case SecurityMode::AWAY:
            return "AWAY";
        default:
            return "UNKNOWN";
    }
}

const char* SecurityController::rejectReasonToString(CommandRejectReason reason) {
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
        case CommandRejectReason::SERVO_ANGLE_OUT_OF_SAFE_RANGE:
            return "servo_angle_out_of_safe_range";
        default:
            return "unknown";
    }
}

StepOutcome SecurityController::makeOutcome(TriggerSource source) const {
    StepOutcome outcome;
    outcome.trigger_source = source;
    outcome.state = state_;
    return outcome;
}

CommandResult SecurityController::makeApplied(const StepOutcome& outcome) const {
    CommandResult res;
    res.status = CommandStatus::APPLIED;
    res.reason = CommandRejectReason::NONE;
    res.outcome = outcome;
    return res;
}

CommandResult SecurityController::makeRejected(CommandRejectReason reason) const {
    CommandResult res;
    res.status = CommandStatus::REJECTED;
    res.reason = reason;
    res.outcome = makeOutcome(TriggerSource::MQTT_COMMAND);
    return res;
}

}  // namespace node_sec
