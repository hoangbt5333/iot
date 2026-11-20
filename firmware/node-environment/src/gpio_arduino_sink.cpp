#include "gpio_arduino_sink.h"

#include <Arduino.h>

namespace node_env {

void ArduinoGpioSink::configureSafeOutput(uint8_t pin, bool initial_high) {
    const uint8_t level = initial_high ? HIGH : LOW;
    // Arduino-ESP32 periman ignores digitalWrite() until the pin is registered as GPIO;
    // setting INPUT first lets digitalWrite() pre-latch the safe level before enabling OUTPUT.
    pinMode(pin, INPUT);
    digitalWrite(pin, level);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
}

void ArduinoGpioSink::writeOutput(uint8_t pin, bool level_high) {
    digitalWrite(pin, level_high ? HIGH : LOW);
}

}  // namespace node_env
