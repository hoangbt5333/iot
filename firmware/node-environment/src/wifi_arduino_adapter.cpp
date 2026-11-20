#include "wifi_arduino_adapter.h"

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFi.h>

#include "node_profile.h"

namespace node_env {
namespace {

#if defined(NODE_ENV_SIM)
static_assert(node_profile::kWifiConfigured,
              "SIM profile must enable Wokwi-GUEST Wi-Fi configuration.");
static_assert(node_profile::kWifiChannel == 6,
              "Wokwi-GUEST simulation must use channel 6 per Wokwi documentation.");
static_assert(node_profile::kWifiPassword[0] == '\0',
              "Wokwi-GUEST simulation uses an empty password.");
#elif defined(NODE_ENV_REAL)
static_assert(!node_profile::kWifiConfigured,
              "REAL profile must default to UNCONFIGURED without hardcoded Wi-Fi credentials.");
static_assert(node_profile::kWifiSsid[0] == '\0' && node_profile::kWifiPassword[0] == '\0',
              "REAL profile must never hardcode SSID or password.");
static_assert(node_profile::kWifiChannel == 0,
              "REAL unconfigured profile must leave channel at 0.");
#endif

WiFiLinkStatus mapArduinoStatus(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:
            return WiFiLinkStatus::IDLE;
        case WL_NO_SSID_AVAIL:
            return WiFiLinkStatus::NO_SSID_AVAIL;
        case WL_CONNECTED:
            return WiFiLinkStatus::CONNECTED;
        case WL_CONNECT_FAILED:
            return WiFiLinkStatus::CONNECT_FAILED;
        case WL_CONNECTION_LOST:
            return WiFiLinkStatus::CONNECTION_LOST;
        case WL_DISCONNECTED:
            return WiFiLinkStatus::DISCONNECTED;
        case WL_STOPPED:
            return WiFiLinkStatus::STOPPED;
        case WL_NO_SHIELD:
        default:
            return WiFiLinkStatus::HARDWARE_UNAVAILABLE;
    }
}

}  // namespace

bool ArduinoWiFiAdapter::configureStation() {
    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_STA)) {
        return false;
    }
    WiFi.setAutoReconnect(false);
    return true;
}

bool ArduinoWiFiAdapter::beginConnect(const char* ssid, const char* password, int32_t channel) {
    if (ssid == nullptr || ssid[0] == '\0') {
        return false;
    }

    const char* safe_password = (password != nullptr) ? password : "";
    if (!WiFi.STA.begin() || !WiFi.STA.connect(ssid, safe_password, channel)) {
        return false;
    }

    awaiting_fresh_status_ = true;
    connect_started_ms_ = millis();
    return true;
}

void ArduinoWiFiAdapter::disconnectAttempt() {
    awaiting_fresh_status_ = false;
    WiFi.disconnectAsync(false, false);
}

WiFiAdapterStatus ArduinoWiFiAdapter::pollStatus() {
    WiFiAdapterStatus out;
    if ((WiFi.getMode() & WIFI_MODE_STA) == 0) {
        out.adapter_ok = false;
        out.link_status = WiFiLinkStatus::STOPPED;
        return out;
    }

    const wl_status_t raw_status = WiFi.status();
    if (raw_status == WL_NO_SHIELD) {
        out.adapter_ok = false;
        out.link_status = WiFiLinkStatus::HARDWARE_UNAVAILABLE;
        return out;
    }

    WiFiLinkStatus mapped = mapArduinoStatus(raw_status);
    if (awaiting_fresh_status_) {
        if (mapped == WiFiLinkStatus::CONNECTED || mapped == WiFiLinkStatus::IDLE ||
            mapped == WiFiLinkStatus::DISCONNECTED) {
            awaiting_fresh_status_ = false;
        } else if ((mapped == WiFiLinkStatus::CONNECT_FAILED ||
                    mapped == WiFiLinkStatus::NO_SSID_AVAIL) &&
                   static_cast<uint32_t>(millis() - connect_started_ms_) < kStaleFailureMaskMs) {
            // Arduino-ESP32 STA._status can retain the prior attempt's failure until the async
            // Wi-Fi event task processes the new connect request.
            mapped = WiFiLinkStatus::IDLE;
        } else {
            awaiting_fresh_status_ = false;
        }
    }

    out.adapter_ok = true;
    out.link_status = mapped;
    if (raw_status == WL_CONNECTED) {
        const IPAddress ip = WiFi.localIP();
        out.has_valid_ip = WiFi.STA.hasIP() && (ip != IPAddress(0, 0, 0, 0));
        if (out.has_valid_ip) {
            out.rssi_dbm = WiFi.RSSI();
        }
    }
    return out;
}

WiFiStationConfig ArduinoWiFiAdapter::defaultConfigFromProfile() {
    WiFiStationConfig cfg;
    cfg.enabled = node_profile::kWifiConfigured;
    cfg.ssid = node_profile::kWifiSsid;
    cfg.password = node_profile::kWifiPassword;
    cfg.channel = node_profile::kWifiChannel;
    cfg.connect_timeout_ms = node_profile::kWifiConnectTimeoutMs;
    cfg.backoff_initial_ms = node_profile::kWifiBackoffInitialMs;
    cfg.backoff_max_ms = node_profile::kWifiBackoffMaxMs;
    return cfg;
}

}  // namespace node_env
