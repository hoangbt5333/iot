#include "light_arduino_driver.h"

#include <Arduino.h>
#include <cmath>

namespace node_env {

#if defined(NODE_ENV_REAL)

Bh1750ArduinoDriver::Bh1750ArduinoDriver(uint8_t sda_pin, uint8_t scl_pin, uint8_t i2c_addr)
    : sda_pin_(sda_pin), scl_pin_(scl_pin), i2c_addr_(i2c_addr), bh1750_(i2c_addr) {}

bool Bh1750ArduinoDriver::begin() {
    if (!wire_started_) {
        Wire.begin(sda_pin_, scl_pin_);
        wire_started_ = true;
    }
    initialized_ = bh1750_.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, i2c_addr_, &Wire);
    return initialized_;
}

LightRawStatus Bh1750ArduinoDriver::readRawLux(float* out_lux) {
    if (out_lux == nullptr) {
        return LightRawStatus::IO_FAILURE;
    }

    if (!initialized_) {
        // Lightweight I2C presence probe avoids repeated Wire resets or BH1750 configure() Serial spam.
        Wire.beginTransmission(i2c_addr_);
        if (Wire.endTransmission() != 0) {
            return LightRawStatus::NOT_INITIALIZED;
        }
        initialized_ = bh1750_.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, i2c_addr_, &Wire);
        if (!initialized_) {
            return LightRawStatus::NOT_INITIALIZED;
        }
    }

    if (!bh1750_.measurementReady(true)) {
        return LightRawStatus::MEASUREMENT_NOT_READY;
    }

    const float level = bh1750_.readLightLevel();
    if (!std::isfinite(level) || level < 0.0f) {
        // Negative sentinel (-1.0 or -2.0) signals I2C read failure or unconfigured device.
        initialized_ = false;
        return LightRawStatus::IO_FAILURE;
    }

    *out_lux = level;
    return LightRawStatus::OK;
}

#elif defined(NODE_ENV_SIM)

LdrAdcArduinoDriver::LdrAdcArduinoDriver(uint8_t adc_pin, float max_valid_lux)
    : adc_pin_(adc_pin), max_valid_lux_(max_valid_lux) {}

bool LdrAdcArduinoDriver::begin() {
    pinMode(adc_pin_, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(adc_pin_, ADC_11db);
    return true;
}

LightRawStatus LdrAdcArduinoDriver::readRawLux(float* out_lux) {
    const int32_t raw_adc = static_cast<int32_t>(analogRead(adc_pin_));
    return LightSensor::convertWokwiLdrAdcToLux(raw_adc, params_, max_valid_lux_, out_lux, nullptr);
}

#endif

}  // namespace node_env
