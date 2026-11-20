#pragma once

#include <stdint.h>

#include "security_outputs.h"

namespace node_sec_arduino {

// Arduino GPIO sink isolating pinMode/digitalWrite from pure C++ alarm output sync logic.
class ArduinoAlarmOutputSink final : public node_sec::IAlarmOutputSink {
public:
    bool configureOutput(uint8_t pin, bool initial_level_high) override;
    bool writeOutput(uint8_t pin, bool level_high) override;

    static node_sec::AlarmOutputConfig defaultOutputConfigFromProfile();
};

}  // namespace node_sec_arduino
