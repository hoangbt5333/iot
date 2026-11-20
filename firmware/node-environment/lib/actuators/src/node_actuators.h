#pragma once

#include <cstdint>

#include "node_automation.h"

namespace node_env {

enum class ActuatorPolarity : uint8_t {
    ACTIVE_HIGH = 0,
    ACTIVE_LOW = 1
};

struct ActuatorPinConfig {
    uint8_t lamp_pin = 26U;
    uint8_t fan_pin = 25U;
    ActuatorPolarity lamp_polarity = ActuatorPolarity::ACTIVE_HIGH;
    ActuatorPolarity fan_polarity = ActuatorPolarity::ACTIVE_HIGH;
};

struct ActuatorOutputs {
    bool initialized = false;
    bool lamp_on = false;
    bool fan_on = false;
    bool lamp_level_high = false;
    bool fan_level_high = false;
};

struct ActuatorSyncOutcome {
    bool lamp_written = false;
    bool fan_written = false;
    bool changed = false;
    ActuatorOutputs outputs{};
};

class IGpioOutputSink {
public:
    virtual ~IGpioOutputSink() = default;
    virtual void configureSafeOutput(uint8_t pin, bool initial_high) = 0;
    virtual void writeOutput(uint8_t pin, bool level_high) = 0;
};

// Pure C++ actuator output synchronizer separating GPIO hardware calls from AutomationController.
class NodeActuatorSync {
public:
    NodeActuatorSync();
    explicit NodeActuatorSync(const ActuatorPinConfig& config);

    void beginSafeOff(IGpioOutputSink& sink);
    ActuatorSyncOutcome sync(const AutomationState& state, IGpioOutputSink& sink);

    const ActuatorPinConfig& config() const { return config_; }
    const ActuatorOutputs& outputs() const { return outputs_; }
    bool isInitialized() const { return outputs_.initialized; }

    static bool logicalToGpioHigh(bool on, ActuatorPolarity polarity);
    static const char* polarityToString(ActuatorPolarity polarity);

private:
    ActuatorPinConfig config_{};
    ActuatorOutputs outputs_{};

    void sanitizeConfig(const ActuatorPinConfig& raw_config);
};

}  // namespace node_env
