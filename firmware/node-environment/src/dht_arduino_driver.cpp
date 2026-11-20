#include "dht_arduino_driver.h"

namespace node_env {

DhtArduinoDriver::DhtArduinoDriver(uint8_t data_pin, uint8_t adafruit_dht_type)
    : dht_(data_pin, adafruit_dht_type) {}

void DhtArduinoDriver::begin() {
    dht_.begin();
}

bool DhtArduinoDriver::readRawSample(float* out_temp_c, float* out_humidity_pct) {
    if (out_temp_c == nullptr || out_humidity_pct == nullptr) {
        return false;
    }
    // Single physical read; subsequent readTemperature/readHumidity reuse cached frame within 2000 ms.
    if (!dht_.read(false)) {
        return false;
    }
    *out_temp_c = dht_.readTemperature(false, false);
    *out_humidity_pct = dht_.readHumidity(false);
    return true;
}

}  // namespace node_env
