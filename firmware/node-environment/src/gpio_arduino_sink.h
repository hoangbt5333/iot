#pragma once

#include <cstdint>

#include "node_actuators.h"

namespace node_env {

class ArduinoGpioSink final : public IGpioOutputSink {
public:
    void configureSafeOutput(uint8_t pin, bool initial_high) override;
    void writeOutput(uint8_t pin, bool level_high) override;
};

}  // namespace node_env
