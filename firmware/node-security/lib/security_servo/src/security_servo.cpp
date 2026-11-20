#include "security_servo.h"

namespace node_sec {

ServoOutputSync::ServoOutputSync() : ServoOutputSync(ServoOutputConfig{}) {}

ServoOutputSync::ServoOutputSync(const ServoOutputConfig& config)
    : config_(config),
      last_applied_angle_deg_(config.safety.initial_angle_deg),
      failed_target_angle_deg_(config.safety.initial_angle_deg) {}

bool ServoOutputSync::isValidServoPwmPin(uint8_t pin) {
    if (pin == ServoOutputConfig::kInvalidPin) {
        return false;
    }
    // Match ESP32 (esp32dev) LEDC PWM-capable GPIO pins supported by ESP32Servo.
    return (pin == 2U) || (pin == 4U) || (pin == 5U) ||
           (pin >= 12U && pin <= 19U) ||
           (pin >= 21U && pin <= 23U) ||
           (pin >= 25U && pin <= 27U) ||
           (pin == 32U) || (pin == 33U);
}

bool ServoOutputSync::isValidConfig(const ServoOutputConfig& config) {
    if (!isValidServoPwmPin(config.servo_pin)) {
        return false;
    }
    if (config.pwm_freq_hz < 40U || config.pwm_freq_hz > 400U) {
        return false;
    }
    if (config.min_pulse_us < ServoOutputConfig::kMinSupportedPulseUs ||
        config.max_pulse_us > ServoOutputConfig::kMaxSupportedPulseUs ||
        config.min_pulse_us >= config.max_pulse_us) {
        return false;
    }
    if (!SecurityController::isValidServoConfig(config.safety)) {
        return false;
    }
    if (config.max_consecutive_failures == 0U) {
        return false;
    }
    return true;
}

const char* ServoOutputSync::errorToString(ServoSyncError error) {
    switch (error) {
        case ServoSyncError::NONE:
            return "none";
        case ServoSyncError::NOT_INITIALIZED:
            return "not_initialized";
        case ServoSyncError::INVALID_CONFIG:
            return "invalid_config";
        case ServoSyncError::ANGLE_OUT_OF_SAFE_RANGE:
            return "angle_out_of_safe_range";
        case ServoSyncError::ATTACH_FAILED:
            return "attach_failed";
        case ServoSyncError::WRITE_FAILED:
            return "write_failed";
        case ServoSyncError::FAULT_LATCHED_RETRY_LIMIT:
            return "fault_latched_retry_limit";
        default:
            return "unknown";
    }
}

bool ServoOutputSync::begin() {
    has_applied_move_ = false;
    last_applied_angle_deg_ = config_.safety.initial_angle_deg;
    fault_latched_ = false;
    failed_target_angle_deg_ = config_.safety.initial_angle_deg;
    consecutive_failures_ = 0U;
    initialized_ = isValidConfig(config_);
    return initialized_;
}

void ServoOutputSync::clearFault() {
    fault_latched_ = false;
    consecutive_failures_ = 0U;
}

ServoOutputSyncOutcome ServoOutputSync::recordFailure(IServoOutputSink& sink,
                                                      ServoSyncError error,
                                                      int16_t target_angle_deg) {
    if (consecutive_failures_ < config_.max_consecutive_failures) {
        ++consecutive_failures_;
    }
    failed_target_angle_deg_ = target_angle_deg;
    if (consecutive_failures_ >= config_.max_consecutive_failures) {
        fault_latched_ = true;
    }

    ServoOutputSyncOutcome outcome;
    outcome.initialized = initialized_;
    outcome.attached = sink.isAttached();
    outcome.move_attempted = true;
    outcome.applied = false;
    outcome.fault_latched = fault_latched_;
    outcome.retry_suppressed = false;
    outcome.error = error;
    outcome.target_angle_deg = target_angle_deg;
    outcome.applied_angle_deg = last_applied_angle_deg_;
    outcome.consecutive_failures = consecutive_failures_;
    return outcome;
}

ServoOutputSyncOutcome ServoOutputSync::sync(IServoOutputSink& sink,
                                             const SecurityController& controller) {
    return syncState(sink, controller.state(), &controller.config().servo);
}

ServoOutputSyncOutcome ServoOutputSync::syncState(IServoOutputSink& sink,
                                                  const SecurityState& state,
                                                  const ServoSafetyConfig* active_safety) {
    ServoOutputSyncOutcome outcome;
    outcome.initialized = initialized_;
    outcome.attached = sink.isAttached();
    outcome.fault_latched = fault_latched_;
    outcome.target_angle_deg = state.servo_angle;
    outcome.applied_angle_deg = last_applied_angle_deg_;
    outcome.consecutive_failures = consecutive_failures_;

    if (!initialized_) {
        outcome.error =
            isValidConfig(config_) ? ServoSyncError::NOT_INITIALIZED : ServoSyncError::INVALID_CONFIG;
        return outcome;
    }

    // Never attach PWM or move servo until SecurityController records an explicit move request.
    if (!state.servo_move_requested) {
        return outcome;
    }

    const int16_t min_deg = config_.safety.min_angle_deg;
    const int16_t max_deg = config_.safety.max_angle_deg;
    if (state.servo_angle < min_deg || state.servo_angle > max_deg) {
        outcome.error = ServoSyncError::ANGLE_OUT_OF_SAFE_RANGE;
        return outcome;
    }

    if (active_safety != nullptr &&
        (state.servo_angle < active_safety->min_angle_deg ||
         state.servo_angle > active_safety->max_angle_deg)) {
        outcome.error = ServoSyncError::ANGLE_OUT_OF_SAFE_RANGE;
        return outcome;
    }

    // Do not repeat Servo.write() across loop iterations for an already applied target angle.
    if (has_applied_move_ && (state.servo_angle == last_applied_angle_deg_)) {
        return outcome;
    }

    // Suppress unbounded retries once consecutive failure limit is reached for this target.
    if (fault_latched_ && (state.servo_angle == failed_target_angle_deg_)) {
        outcome.retry_suppressed = true;
        outcome.error = ServoSyncError::FAULT_LATCHED_RETRY_LIMIT;
        return outcome;
    }

    if (!sink.isAttached()) {
        const bool attach_ok = sink.attach(config_.servo_pin,
                                           config_.pwm_freq_hz,
                                           config_.min_pulse_us,
                                           config_.max_pulse_us);
        if (!attach_ok || !sink.isAttached()) {
            return recordFailure(sink, ServoSyncError::ATTACH_FAILED, state.servo_angle);
        }
    }

    if (!sink.writeAngle(state.servo_angle)) {
        return recordFailure(sink, ServoSyncError::WRITE_FAILED, state.servo_angle);
    }

    has_applied_move_ = true;
    last_applied_angle_deg_ = state.servo_angle;
    fault_latched_ = false;
    consecutive_failures_ = 0U;

    outcome.attached = sink.isAttached();
    outcome.move_attempted = true;
    outcome.applied = true;
    outcome.fault_latched = false;
    outcome.error = ServoSyncError::NONE;
    outcome.applied_angle_deg = last_applied_angle_deg_;
    outcome.consecutive_failures = 0U;
    return outcome;
}

}  // namespace node_sec
