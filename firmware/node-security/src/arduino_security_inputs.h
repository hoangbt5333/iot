#pragma once

#include "security_inputs.h"

namespace node_sec_arduino {

// Arduino GPIO adapter isolating pinMode/digitalRead from pure C++ security logic.
class ArduinoSecurityInputSource final : public node_sec::ISecurityInputSource {
public:
    explicit ArduinoSecurityInputSource(const node_sec::SecurityInputPinConfig& pins);

    bool begin() override;
    node_sec::RawSecurityInputSample readInputs() override;

    bool isInitialized() const { return initialized_; }
    const node_sec::SecurityInputPinConfig& pins() const { return pins_; }

    static node_sec::SecurityInputPinConfig defaultPinConfigFromProfile();
    static node_sec::SecurityInputSamplerConfig defaultSamplerConfigFromProfile();

private:
    bool isValidPinConfig() const;

    node_sec::SecurityInputPinConfig pins_{};
    bool initialized_ = false;
};

}  // namespace node_sec_arduino
