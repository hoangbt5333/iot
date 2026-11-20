#pragma once

#include <cstdint>

namespace node_sec {

enum class SecurityMode : uint8_t {
    HOME = 0,
    AWAY = 1
};

enum class TriggerSource : uint8_t {
    SYSTEM_BOOT = 0,
    REED_SWITCH = 1,
    PIR = 2,
    PHYSICAL_SWITCH = 3,
    MQTT_COMMAND = 4,
    EDGE_RULE = 5
};

enum class SecurityEventType : uint8_t {
    NONE = 0,
    DOOR_CHANGED = 1,
    MOTION_DETECTED = 2,
    ALARM_TRIGGERED = 3,
    ALARM_CLEARED = 4,
    SECURITY_MODE_CHANGED = 5
};

enum class CommandTarget : uint8_t {
    UNKNOWN = 0,
    SECURITY_MODE = 1,
    SERVO = 2,
    ALARM = 3
};

enum class CommandAction : uint8_t {
    UNKNOWN = 0,
    SET = 1,
    SILENCE = 2
};

enum class CommandStatus : uint8_t {
    APPLIED = 0,
    REJECTED = 1
};

enum class CommandRejectReason : uint8_t {
    NONE = 0,
    INVALID_TARGET = 1,
    INVALID_ACTION = 2,
    INVALID_MODE = 3,
    INVALID_VALUE = 4,
    SERVO_ANGLE_OUT_OF_SAFE_RANGE = 5
};

// Decoupled switch arbitration policy (PROP-06 remains unapproved by default).
enum class SwitchArbitrationMode : uint8_t {
    DISABLED_PENDING_APPROVAL = 0,
    EDGE_TRANSITION_LAST_WRITER_WINS = 1,
    PHYSICAL_LEVEL_OVERRIDES = 2
};

struct ServoSafetyConfig {
    int16_t min_angle_deg = 0;
    int16_t max_angle_deg = 90;
    int16_t initial_angle_deg = 0;
};

struct SecurityPolicyConfig {
    bool pir_triggers_alarm_in_away = false;  // PROP-06: default false until approved
    SwitchArbitrationMode switch_arbitration = SwitchArbitrationMode::DISABLED_PENDING_APPROVAL;
    uint32_t door_debounce_ms = 50UL;
    uint32_t switch_debounce_ms = 50UL;
    ServoSafetyConfig servo{};
};

struct SecurityState {
    SecurityMode mode = SecurityMode::HOME;
    bool door_open = false;
    bool motion_detected = false;
    bool alarm_on = false;
    int16_t servo_angle = 0;
    bool servo_move_requested = false;
};

struct StepOutcome {
    bool door_changed = false;
    bool motion_rising_edge = false;
    bool alarm_triggered = false;
    bool alarm_cleared = false;
    bool mode_changed = false;
    bool servo_moved = false;
    TriggerSource trigger_source = TriggerSource::SYSTEM_BOOT;
    SecurityState state{};
};

struct CommandResult {
    CommandStatus status = CommandStatus::REJECTED;
    CommandRejectReason reason = CommandRejectReason::NONE;
    StepOutcome outcome{};
};

struct NodeBCommand {
    CommandTarget target = CommandTarget::UNKNOWN;
    CommandAction action = CommandAction::UNKNOWN;
    SecurityMode mode_value = SecurityMode::HOME;
    bool has_mode_value = false;
    bool mode_value_invalid = false;
    int32_t int_value = 0;
    bool has_int_value = false;
    bool bool_value = false;
    bool has_bool_value = false;
};

// Non-blocking debounce helper using unsigned subtraction for 32-bit millis() rollover safety.
class DebouncedInput {
public:
    explicit DebouncedInput(bool initial_stable = false, uint32_t debounce_ms = 50UL);

    bool update(bool raw_state, uint32_t now_ms);
    void reset(bool stable_state, uint32_t now_ms);

    bool stableState() const { return stable_state_; }
    bool rawState() const { return last_raw_state_; }
    uint32_t debounceMs() const { return debounce_ms_; }

private:
    bool stable_state_ = false;
    bool last_raw_state_ = false;
    bool pending_change_ = false;
    uint32_t last_transition_ms_ = 0;
    uint32_t debounce_ms_ = 50UL;
};

class SecurityController {
public:
    static constexpr int16_t kAbsoluteMinServoDeg = 0;
    static constexpr int16_t kAbsoluteMaxServoDeg = 180;

    SecurityController();
    explicit SecurityController(const SecurityPolicyConfig& config);

    const SecurityState& state() const { return state_; }
    const SecurityPolicyConfig& config() const { return config_; }

    // Samples door reed switch with non-blocking debounce; triggers alarm in AWAY on valid open.
    StepOutcome updateDoorInput(bool raw_door_open, uint32_t now_ms);

    // Samples PIR sensor; emits motion_rising_edge once per LOW->HIGH transition.
    StepOutcome updateMotionInput(bool pir_high);

    // Evaluates optional physical Home/Away switch according to decoupled SwitchArbitrationMode.
    StepOutcome updatePhysicalModeSwitch(bool raw_switch_away, uint32_t now_ms);

    // Explicit mode/alarm/servo actions with validation.
    CommandResult setMode(SecurityMode new_mode, TriggerSource source = TriggerSource::MQTT_COMMAND);
    CommandResult setModeFromString(const char* mode_str, TriggerSource source = TriggerSource::MQTT_COMMAND);
    CommandResult setServoAngle(int32_t angle_deg);
    CommandResult setAlarm(bool enable, TriggerSource source = TriggerSource::MQTT_COMMAND);
    CommandResult applyCommand(const NodeBCommand& cmd);

    static bool isValidServoConfig(const ServoSafetyConfig& servo);
    static bool isValidMode(SecurityMode mode);
    static bool parseMode(const char* mode_str, SecurityMode* out_mode);
    static const char* modeToString(SecurityMode mode);
    static const char* rejectReasonToString(CommandRejectReason reason);

private:
    SecurityPolicyConfig config_{};
    SecurityState state_{};
    DebouncedInput door_debouncer_;
    DebouncedInput switch_debouncer_;

    void sanitizeConfig(const SecurityPolicyConfig& raw_config);
    StepOutcome makeOutcome(TriggerSource source) const;
    CommandResult makeApplied(const StepOutcome& outcome) const;
    CommandResult makeRejected(CommandRejectReason reason) const;
};

}  // namespace node_sec
