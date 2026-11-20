#pragma once

#include <ESP32Servo.h>
#include <stdint.h>

#include "security_servo.h"

namespace node_sec_arduino {

// Arduino/ESP32Servo PWM sink isolating LEDC attach/write from pure C++ ServoOutputSync.
class ArduinoServoOutputSink final : public node_sec::IServoOutputSink {
public:
    bool isAttached() const override;
    bool attach(uint8_t pin,
                uint16_t pwm_freq_hz,
                uint16_t min_pulse_us,
                uint16_t max_pulse_us) override;
    bool writeAngle(int16_t angle_deg) override;
    void detach() override;

    static node_sec::ServoOutputConfig defaultConfigFromProfile();

private:
    mutable Servo servo_{};
};

}  // namespace node_sec_arduino
