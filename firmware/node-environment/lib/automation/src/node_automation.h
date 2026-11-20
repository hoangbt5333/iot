#pragma once

#include <cstdint>

namespace node_env {

enum class DeviceMode : uint8_t {
    AUTO = 0,
    MANUAL = 1
};

enum class SensorHealth : uint8_t {
    UNINITIALIZED = 0,
    OK = 1,
    DEGRADED = 2,
    FAULT = 3
};

enum class CommandTarget : uint8_t {
    UNKNOWN = 0,
    LAMP = 1,
    FAN = 2,
    DEVICE_MODE = 3,
    THRESHOLDS = 4
};

enum class CommandAction : uint8_t {
    UNKNOWN = 0,
    SET = 1,
    UPDATE = 2
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
    CONTRADICTORY_MODE = 5,
    INVALID_THRESHOLDS = 6
};

struct AutomationThresholds {
    float light_on_lux = 100.0f;
    float light_off_lux = 150.0f;
    float fan_on_temp_c = 30.0f;
    float fan_off_temp_c = 28.0f;
};

struct SensorReading {
    float temperature_c = 0.0f;
    bool temperature_valid = false;
    float light_lux = 0.0f;
    bool light_lux_valid = false;
};

struct AutomationState {
    DeviceMode mode = DeviceMode::AUTO;
    bool lamp_on = false;
    bool fan_on = false;
    SensorHealth sensor_health = SensorHealth::UNINITIALIZED;
    AutomationThresholds thresholds{};
};

struct CommandResult {
    CommandStatus status = CommandStatus::REJECTED;
    CommandRejectReason reason = CommandRejectReason::NONE;
    AutomationState state{};
};

struct NodeACommand {
    CommandTarget target = CommandTarget::UNKNOWN;
    CommandAction action = CommandAction::UNKNOWN;
    bool bool_value = false;
    bool has_bool_value = false;
    DeviceMode mode_value = DeviceMode::AUTO;
    bool has_mode_value = false;
    bool mode_value_invalid = false;
    AutomationThresholds thresholds_value{};
    bool has_thresholds_value = false;
};

class AutomationController {
public:
    static constexpr float kMinValidTempC = -50.0f;
    static constexpr float kMaxValidTempC = 125.0f;
    static constexpr float kMinValidLux = 0.0f;

    AutomationController();
    explicit AutomationController(const AutomationThresholds& initial_thresholds);

    const AutomationState& state() const { return state_; }

    // Evaluates physical readings (temperature_c and light_lux). Invalid/NaN inputs keep current actuator state.
    AutomationState evaluateSensors(const SensorReading& reading);

    CommandResult setMode(DeviceMode new_mode);
    CommandResult setModeFromString(const char* mode_str);
    CommandResult setLampManual(bool on);
    CommandResult setFanManual(bool on);
    CommandResult setThresholds(const AutomationThresholds& thresholds);
    CommandResult applyCommand(const NodeACommand& cmd);

    static bool isValidThresholds(const AutomationThresholds& thresholds);
    static bool isValidTemperatureReading(float temp_c, bool valid_flag);
    static bool isValidLuxReading(float lux, bool valid_flag);
    static bool parseMode(const char* mode_str, DeviceMode* out_mode);
    static const char* modeToString(DeviceMode mode);
    static const char* healthToString(SensorHealth health);
    static const char* rejectReasonToString(CommandRejectReason reason);

private:
    AutomationState state_{};

    void updateSensorHealth(bool temp_ok, bool light_ok);
    void evaluateLampAuto(float lux);
    void evaluateFanAuto(float temp_c);
    CommandResult makeApplied() const;
    CommandResult makeRejected(CommandRejectReason reason) const;
};

}  // namespace node_env
