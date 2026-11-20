#pragma once

#include <cstdint>

#if defined(NODE_ENV_REAL)
#include <BH1750.h>
#include <Wire.h>
#endif

#include "light_sensor.h"

namespace node_env {

#if defined(NODE_ENV_REAL)
class Bh1750ArduinoDriver final : public ILightRawReader {
public:
    Bh1750ArduinoDriver(uint8_t sda_pin, uint8_t scl_pin, uint8_t i2c_addr);

    bool begin();
    bool isInitialized() const { return initialized_; }
    LightRawStatus readRawLux(float* out_lux) override;

private:
    uint8_t sda_pin_;
    uint8_t scl_pin_;
    uint8_t i2c_addr_;
    BH1750 bh1750_;
    bool wire_started_ = false;
    bool initialized_ = false;
};
#elif defined(NODE_ENV_SIM)
class LdrAdcArduinoDriver final : public ILightRawReader {
public:
    explicit LdrAdcArduinoDriver(uint8_t adc_pin, float max_valid_lux = LightSensor::kDefaultMaxValidLux);

    bool begin();
    LightRawStatus readRawLux(float* out_lux) override;

private:
    uint8_t adc_pin_;
    float max_valid_lux_;
    WokwiLdrConversionParams params_{};
};
#endif

}  // namespace node_env
