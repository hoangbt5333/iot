#include "arduino_security_outputs.h"

#include <Arduino.h>

#include "node_profile.h"

namespace node_sec_arduino {

static_assert(node_profile::kBuzzerOutputPin == 18U,
              "Node B buzzer signal must stay on GPIO18 per hardware plan.");
static_assert(node_profile::kAlarmLedOutputPin == 26U,
              "Node B alarm LED must stay on GPIO26 per hardware plan.");
static_assert(node_profile::kBuzzerActiveHigh,
              "Node B buzzer assumes ACTIVE_HIGH by default until physical module is verified.");
static_assert(node_profile::kAlarmLedActiveHigh,
              "Node B alarm LED assumes ACTIVE_HIGH by default until physical module is verified.");

bool ArduinoAlarmOutputSink::configureOutput(uint8_t pin, bool initial_level_high) {
    if (!node_sec::AlarmOutputSync::isValidOutputPin(pin)) {
        return false;
    }

    // Write safe output latch before and after OUTPUT direction setup in setup().
    // Note: GPIO state during pre-setup ROM boot/reset cannot be guaranteed by firmware.
    const uint8_t level = initial_level_high ? HIGH : LOW;
    digitalWrite(pin, level);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
    return true;
}

bool ArduinoAlarmOutputSink::writeOutput(uint8_t pin, bool level_high) {
    if (!node_sec::AlarmOutputSync::isValidOutputPin(pin)) {
        return false;
    }

    digitalWrite(pin, level_high ? HIGH : LOW);
    return true;
}

node_sec::AlarmOutputConfig ArduinoAlarmOutputSink::defaultOutputConfigFromProfile() {
    node_sec::AlarmOutputConfig cfg;
    cfg.pins.buzzer_pin = node_profile::kBuzzerOutputPin;
    cfg.pins.alarm_led_pin = node_profile::kAlarmLedOutputPin;
    cfg.polarity.buzzer_active_high = node_profile::kBuzzerActiveHigh;
    cfg.polarity.alarm_led_active_high = node_profile::kAlarmLedActiveHigh;
    return cfg;
}

}  // namespace node_sec_arduino
