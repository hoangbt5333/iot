#pragma once

#include <DHT.h>
#include <cstdint>

#include "temp_humidity_sensor.h"

namespace node_env {

// Arduino/ESP32 adapter wrapping adafruit/DHT sensor library.
// Note: DHT::read() uses synchronous single-wire pulse timing (~5-25 ms) and brief InterruptLock.
class DhtArduinoDriver final : public IDhtRawReader {
public:
    DhtArduinoDriver(uint8_t data_pin, uint8_t adafruit_dht_type);

    void begin();
    bool readRawSample(float* out_temp_c, float* out_humidity_pct) override;

private:
    DHT dht_;
};

}  // namespace node_env
