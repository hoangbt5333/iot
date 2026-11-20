#pragma once

#include <cstdint>

#include "wifi_connection_manager.h"

namespace node_env {

// Arduino-ESP32 Wi-Fi STA adapter isolating WiFi.h calls from WiFiConnectionManager.
class ArduinoWiFiAdapter final : public IWiFiAdapter {
public:
    bool configureStation() override;
    bool beginConnect(const char* ssid, const char* password, int32_t channel) override;
    void disconnectAttempt() override;
    WiFiAdapterStatus pollStatus() override;

    static WiFiStationConfig defaultConfigFromProfile();

private:
    static constexpr uint32_t kStaleFailureMaskMs = 500UL;
    bool awaiting_fresh_status_ = false;
    uint32_t connect_started_ms_ = 0UL;
};

}  // namespace node_env
