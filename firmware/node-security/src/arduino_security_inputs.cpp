#include "arduino_security_inputs.h"

#include <Arduino.h>

#include "node_profile.h"

namespace node_sec_arduino {

static_assert(node_profile::kPirInputPin == 27U,
              "Node B PIR must stay on GPIO27 per firmware plan.");
static_assert(node_profile::kReedSwitchInputPin == 32U,
              "Node B reed switch must stay on GPIO32 per firmware plan.");
static_assert(node_profile::kPirMotionActiveHigh,
              "Node B PIR polarity must be centralized as active-HIGH.");
static_assert(node_profile::kReedDoorOpenHigh,
              "Node B reed switch with INPUT_PULLUP must treat HIGH as OPEN and LOW as CLOSED.");
static_assert(node_profile::kReedUseInternalPullup,
              "Node B reed switch on GPIO32 must enable INPUT_PULLUP.");

ArduinoSecurityInputSource::ArduinoSecurityInputSource(
    const node_sec::SecurityInputPinConfig& pins)
    : pins_(pins) {}

bool ArduinoSecurityInputSource::isValidPinConfig() const {
    if (pins_.pir_pin == node_sec::SecurityInputPinConfig::kInvalidPin ||
        pins_.reed_pin == node_sec::SecurityInputPinConfig::kInvalidPin ||
        pins_.pir_pin == pins_.reed_pin) {
        return false;
    }
    // ESP32 GPIO34-39 are input-only without internal pull-up resistors.
    if (pins_.reed_use_internal_pullup && pins_.reed_pin >= 34U && pins_.reed_pin <= 39U) {
        return false;
    }
    return true;
}

bool ArduinoSecurityInputSource::begin() {
    if (!isValidPinConfig()) {
        initialized_ = false;
        return false;
    }

    // Configure GPIO27 INPUT and GPIO32 INPUT_PULLUP strictly before any read.
    pinMode(pins_.pir_pin, INPUT);
    pinMode(pins_.reed_pin, pins_.reed_use_internal_pullup ? INPUT_PULLUP : INPUT);
    initialized_ = true;
    return true;
}

node_sec::RawSecurityInputSample ArduinoSecurityInputSource::readInputs() {
    node_sec::RawSecurityInputSample sample;
    if (!initialized_) {
        return sample;
    }

    sample.pir_level_high = (digitalRead(pins_.pir_pin) == HIGH);
    sample.reed_level_high = (digitalRead(pins_.reed_pin) == HIGH);
    sample.valid = true;
    return sample;
}

node_sec::SecurityInputPinConfig ArduinoSecurityInputSource::defaultPinConfigFromProfile() {
    node_sec::SecurityInputPinConfig cfg;
    cfg.pir_pin = node_profile::kPirInputPin;
    cfg.reed_pin = node_profile::kReedSwitchInputPin;
    cfg.reed_use_internal_pullup = node_profile::kReedUseInternalPullup;
    return cfg;
}

node_sec::SecurityInputSamplerConfig
ArduinoSecurityInputSource::defaultSamplerConfigFromProfile() {
    node_sec::SecurityInputSamplerConfig cfg;
    cfg.polarity.pir_motion_active_high = node_profile::kPirMotionActiveHigh;
    cfg.polarity.reed_door_open_high = node_profile::kReedDoorOpenHigh;
    cfg.sample_interval_ms = node_profile::kInputSampleIntervalMs;
    return cfg;
}

}  // namespace node_sec_arduino
