#include "node_actuators.h"

namespace node_env {

NodeActuatorSync::NodeActuatorSync() : NodeActuatorSync(ActuatorPinConfig{}) {}

NodeActuatorSync::NodeActuatorSync(const ActuatorPinConfig& config) {
    sanitizeConfig(config);
    outputs_.initialized = false;
    outputs_.lamp_on = false;
    outputs_.fan_on = false;
    outputs_.lamp_level_high = logicalToGpioHigh(false, config_.lamp_polarity);
    outputs_.fan_level_high = logicalToGpioHigh(false, config_.fan_polarity);
}

void NodeActuatorSync::sanitizeConfig(const ActuatorPinConfig& raw_config) {
    config_.lamp_pin = raw_config.lamp_pin;
    config_.fan_pin = raw_config.fan_pin;
    config_.lamp_polarity = (raw_config.lamp_polarity == ActuatorPolarity::ACTIVE_LOW)
                                ? ActuatorPolarity::ACTIVE_LOW
                                : ActuatorPolarity::ACTIVE_HIGH;
    config_.fan_polarity = (raw_config.fan_polarity == ActuatorPolarity::ACTIVE_LOW)
                               ? ActuatorPolarity::ACTIVE_LOW
                               : ActuatorPolarity::ACTIVE_HIGH;
}

void NodeActuatorSync::beginSafeOff(IGpioOutputSink& sink) {
    const bool lamp_off_high = logicalToGpioHigh(false, config_.lamp_polarity);
    const bool fan_off_high = logicalToGpioHigh(false, config_.fan_polarity);

    sink.configureSafeOutput(config_.lamp_pin, lamp_off_high);
    sink.configureSafeOutput(config_.fan_pin, fan_off_high);

    outputs_.initialized = true;
    outputs_.lamp_on = false;
    outputs_.fan_on = false;
    outputs_.lamp_level_high = lamp_off_high;
    outputs_.fan_level_high = fan_off_high;
}

ActuatorSyncOutcome NodeActuatorSync::sync(const AutomationState& state, IGpioOutputSink& sink) {
    if (!outputs_.initialized) {
        beginSafeOff(sink);
    }

    ActuatorSyncOutcome outcome{};
    const bool target_lamp_high = logicalToGpioHigh(state.lamp_on, config_.lamp_polarity);
    const bool target_fan_high = logicalToGpioHigh(state.fan_on, config_.fan_polarity);

    if (state.lamp_on != outputs_.lamp_on || target_lamp_high != outputs_.lamp_level_high) {
        sink.writeOutput(config_.lamp_pin, target_lamp_high);
        outputs_.lamp_on = state.lamp_on;
        outputs_.lamp_level_high = target_lamp_high;
        outcome.lamp_written = true;
    }

    if (state.fan_on != outputs_.fan_on || target_fan_high != outputs_.fan_level_high) {
        sink.writeOutput(config_.fan_pin, target_fan_high);
        outputs_.fan_on = state.fan_on;
        outputs_.fan_level_high = target_fan_high;
        outcome.fan_written = true;
    }

    outcome.changed = outcome.lamp_written || outcome.fan_written;
    outcome.outputs = outputs_;
    return outcome;
}

bool NodeActuatorSync::logicalToGpioHigh(bool on, ActuatorPolarity polarity) {
    if (polarity == ActuatorPolarity::ACTIVE_LOW) {
        return !on;
    }
    return on;
}

const char* NodeActuatorSync::polarityToString(ActuatorPolarity polarity) {
    switch (polarity) {
        case ActuatorPolarity::ACTIVE_LOW:
            return "ACTIVE_LOW";
        case ActuatorPolarity::ACTIVE_HIGH:
        default:
            return "ACTIVE_HIGH";
    }
}

}  // namespace node_env
