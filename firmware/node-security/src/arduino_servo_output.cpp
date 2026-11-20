#include "arduino_servo_output.h"

#include "node_profile.h"

namespace node_sec_arduino {

static_assert(node_profile::kServoPwmPin == 19U,
              "Node B servo PWM signal must stay on GPIO19 per hardware plan.");
static_assert(node_profile::kServoPwmFreqHz == 50U,
              "Node B SG90 servo expects 50Hz PWM frequency.");
static_assert(node_profile::kServoMinPulseUs >= MIN_PULSE_WIDTH &&
                  node_profile::kServoMaxPulseUs <= MAX_PULSE_WIDTH &&
                  node_profile::kServoMinPulseUs < node_profile::kServoMaxPulseUs,
              "Servo pulse range must stay within ESP32Servo library limits [500, 2500] us.");
static_assert(node_profile::kServoMinAngleDeg == 0 &&
                  node_profile::kServoMaxAngleDeg == 90 &&
                  node_profile::kServoInitialAngleDeg == 0,
              "Default Node B servo safe angle range [0, 90] must remain preserved.");

bool ArduinoServoOutputSink::isAttached() const {
    return servo_.attached();
}

bool ArduinoServoOutputSink::attach(uint8_t pin,
                                    uint16_t pwm_freq_hz,
                                    uint16_t min_pulse_us,
                                    uint16_t max_pulse_us) {
    if (!node_sec::ServoOutputSync::isValidServoPwmPin(pin)) {
        return false;
    }
    if (min_pulse_us < MIN_PULSE_WIDTH || max_pulse_us > MAX_PULSE_WIDTH ||
        min_pulse_us >= max_pulse_us) {
        return false;
    }

    servo_.setPeriodHertz(static_cast<int>(pwm_freq_hz));
    // Note: ESP32Servo::attach() returns the assigned LEDC channel (0 is valid for channel 0),
    // so attachment success MUST be verified via servo_.attached().
    (void)servo_.attach(static_cast<int>(pin),
                        static_cast<int>(min_pulse_us),
                        static_cast<int>(max_pulse_us));
    return servo_.attached();
}

bool ArduinoServoOutputSink::writeAngle(int16_t angle_deg) {
    if (!servo_.attached()) {
        return false;
    }
    servo_.write(static_cast<int>(angle_deg));
    return true;
}

void ArduinoServoOutputSink::detach() {
    if (servo_.attached()) {
        servo_.detach();
    }
}

node_sec::ServoOutputConfig ArduinoServoOutputSink::defaultConfigFromProfile() {
    node_sec::ServoOutputConfig cfg;
    cfg.servo_pin = node_profile::kServoPwmPin;
    cfg.pwm_freq_hz = node_profile::kServoPwmFreqHz;
    cfg.min_pulse_us = node_profile::kServoMinPulseUs;
    cfg.max_pulse_us = node_profile::kServoMaxPulseUs;
    cfg.safety.min_angle_deg = node_profile::kServoMinAngleDeg;
    cfg.safety.max_angle_deg = node_profile::kServoMaxAngleDeg;
    cfg.safety.initial_angle_deg = node_profile::kServoInitialAngleDeg;
    return cfg;
}

}  // namespace node_sec_arduino
