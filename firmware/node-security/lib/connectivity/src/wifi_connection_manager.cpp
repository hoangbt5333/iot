#include "wifi_connection_manager.h"

#include <cstring>

namespace node_sec {
namespace {

uint32_t sanitizeDuration(uint32_t raw_ms, uint32_t default_ms, uint32_t min_ms) {
    if (raw_ms == 0UL) {
        return default_ms;
    }
    return (raw_ms < min_ms) ? min_ms : raw_ms;
}

bool copyBoundedCString(const char* src, char* dst, size_t max_len) {
    dst[0] = '\0';
    if (src == nullptr) {
        return true;
    }
    const size_t len = std::strlen(src);
    if (len > max_len) {
        return false;
    }
    if (len > 0U) {
        std::memcpy(dst, src, len);
    }
    dst[len] = '\0';
    return true;
}

}  // namespace

WiFiConnectionManager::WiFiConnectionManager() {
    configure(WiFiStationConfig{});
}

WiFiConnectionManager::WiFiConnectionManager(const WiFiStationConfig& config) {
    configure(config);
}

void WiFiConnectionManager::configure(const WiFiStationConfig& config) {
    sanitizeAndStoreConfig(config);
}

void WiFiConnectionManager::sanitizeAndStoreConfig(const WiFiStationConfig& raw) {
    std::memset(ssid_buf_, 0, sizeof(ssid_buf_));
    std::memset(password_buf_, 0, sizeof(password_buf_));
    station_configured_ = false;
    attempt_started_ms_ = 0UL;
    backoff_started_ms_ = 0UL;
    snapshot_ = WiFiStatusSnapshot{};

    connect_timeout_ms_ =
        sanitizeDuration(raw.connect_timeout_ms, kDefaultConnectTimeoutMs, kMinConnectTimeoutMs);
    backoff_initial_ms_ =
        sanitizeDuration(raw.backoff_initial_ms, kDefaultBackoffInitialMs, kMinBackoffMs);
    backoff_max_ms_ =
        sanitizeDuration(raw.backoff_max_ms, kDefaultBackoffMaxMs, kMinBackoffMs);
    if (backoff_max_ms_ < backoff_initial_ms_) {
        backoff_max_ms_ = backoff_initial_ms_;
    }

    if (!raw.enabled) {
        channel_ = 0;
        snapshot_.state = WiFiState::UNCONFIGURED;
        snapshot_.last_error = WiFiError::UNCONFIGURED;
        snapshot_.configured = false;
        return;
    }

    const bool ssid_len_ok = copyBoundedCString(raw.ssid, ssid_buf_, kMaxSsidLen);
    const bool pass_len_ok = copyBoundedCString(raw.password, password_buf_, kMaxPasswordLen);
    const bool channel_ok = (raw.channel >= kMinChannel && raw.channel <= kMaxChannel);
    const bool ssid_present = (ssid_buf_[0] != '\0');

    if (!ssid_len_ok || !pass_len_ok || !channel_ok || !ssid_present) {
        std::memset(ssid_buf_, 0, sizeof(ssid_buf_));
        std::memset(password_buf_, 0, sizeof(password_buf_));
        channel_ = 0;
        snapshot_.state = WiFiState::CONFIG_ERROR;
        snapshot_.last_error = WiFiError::INVALID_CONFIG;
        snapshot_.configured = false;
        return;
    }

    channel_ = raw.channel;
    snapshot_.configured = true;
    snapshot_.state = WiFiState::DISCONNECTED;
    snapshot_.last_error = WiFiError::NONE;
}

void WiFiConnectionManager::begin(IWiFiAdapter& adapter) {
    if (snapshot_.state != WiFiState::DISCONNECTED) {
        return;
    }
    station_configured_ = adapter.configureStation();
    if (!station_configured_) {
        snapshot_.last_error = WiFiError::ADAPTER_INIT_FAILED;
    }
}

void WiFiConnectionManager::requestDisconnect(IWiFiAdapter& adapter) {
    if (snapshot_.state == WiFiState::UNCONFIGURED ||
        snapshot_.state == WiFiState::CONFIG_ERROR) {
        return;
    }
    if (snapshot_.state == WiFiState::CONNECTING ||
        snapshot_.state == WiFiState::CONNECTED) {
        adapter.disconnectAttempt();
    }
    snapshot_.state = WiFiState::DISCONNECTED;
    snapshot_.connected = false;
    snapshot_.has_valid_ip = false;
    snapshot_.rssi_dbm = 0;
    snapshot_.consecutive_failures = 0UL;
    snapshot_.current_backoff_ms = 0UL;
    snapshot_.last_error = WiFiError::NONE;
}

WiFiStepOutcome WiFiConnectionManager::tick(uint32_t now_ms, IWiFiAdapter& adapter) {
    WiFiStepOutcome outcome;
    outcome.previous_state = snapshot_.state;

    switch (snapshot_.state) {
        case WiFiState::UNCONFIGURED:
        case WiFiState::CONFIG_ERROR:
            break;

        case WiFiState::DISCONNECTED:
            startConnectAttempt(now_ms, adapter, &outcome.connect_attempted);
            break;

        case WiFiState::CONNECTING: {
            const WiFiAdapterStatus st = adapter.pollStatus();
            if (!st.adapter_ok || st.link_status == WiFiLinkStatus::HARDWARE_UNAVAILABLE) {
                adapter.disconnectAttempt();
                station_configured_ = false;
                if (snapshot_.connect_failures < 0xFFFFFFFFUL) {
                    ++snapshot_.connect_failures;
                }
                enterBackoff(now_ms, WiFiError::ADAPTER_INIT_FAILED);
                break;
            }

            if (st.link_status == WiFiLinkStatus::CONNECTED && st.has_valid_ip) {
                snapshot_.rssi_dbm = st.rssi_dbm;
                enterConnected();
                break;
            }

            if (st.link_status == WiFiLinkStatus::NO_SSID_AVAIL ||
                st.link_status == WiFiLinkStatus::CONNECT_FAILED) {
                adapter.disconnectAttempt();
                if (snapshot_.connect_failures < 0xFFFFFFFFUL) {
                    ++snapshot_.connect_failures;
                }
                enterBackoff(now_ms, WiFiError::LINK_FAILED);
                break;
            }

            const uint32_t elapsed_ms = static_cast<uint32_t>(now_ms - attempt_started_ms_);
            if (elapsed_ms >= connect_timeout_ms_) {
                adapter.disconnectAttempt();
                if (snapshot_.connect_failures < 0xFFFFFFFFUL) {
                    ++snapshot_.connect_failures;
                }
                const WiFiError timeout_err =
                    (st.link_status == WiFiLinkStatus::CONNECTED && !st.has_valid_ip)
                        ? WiFiError::CONNECTED_WITHOUT_IP
                        : WiFiError::CONNECT_TIMEOUT;
                enterBackoff(now_ms, timeout_err);
            }
            break;
        }

        case WiFiState::CONNECTED: {
            const WiFiAdapterStatus st = adapter.pollStatus();
            if (!st.adapter_ok || st.link_status != WiFiLinkStatus::CONNECTED ||
                !st.has_valid_ip) {
                adapter.disconnectAttempt();
                if (!st.adapter_ok || st.link_status == WiFiLinkStatus::HARDWARE_UNAVAILABLE) {
                    station_configured_ = false;
                }
                if (snapshot_.disconnect_count < 0xFFFFFFFFUL) {
                    ++snapshot_.disconnect_count;
                }
                const WiFiError drop_err =
                    (!st.adapter_ok || st.link_status == WiFiLinkStatus::HARDWARE_UNAVAILABLE)
                        ? WiFiError::ADAPTER_INIT_FAILED
                        : WiFiError::LINK_LOST;
                enterBackoff(now_ms, drop_err);
            } else {
                snapshot_.rssi_dbm = st.rssi_dbm;
            }
            break;
        }

        case WiFiState::BACKOFF: {
            const uint32_t elapsed_ms = static_cast<uint32_t>(now_ms - backoff_started_ms_);
            if (elapsed_ms >= snapshot_.current_backoff_ms) {
                startConnectAttempt(now_ms, adapter, &outcome.connect_attempted);
            }
            break;
        }
    }

    outcome.current_state = snapshot_.state;
    outcome.state_changed = (outcome.current_state != outcome.previous_state);
    outcome.snapshot = snapshot_;
    return outcome;
}

void WiFiConnectionManager::startConnectAttempt(uint32_t now_ms,
                                                IWiFiAdapter& adapter,
                                                bool* out_attempted) {
    if (!station_configured_) {
        station_configured_ = adapter.configureStation();
        if (!station_configured_) {
            if (snapshot_.connect_failures < 0xFFFFFFFFUL) {
                ++snapshot_.connect_failures;
            }
            enterBackoff(now_ms, WiFiError::ADAPTER_INIT_FAILED);
            return;
        }
    }

    if (snapshot_.connect_attempts < 0xFFFFFFFFUL) {
        ++snapshot_.connect_attempts;
    }
    if (out_attempted != nullptr) {
        *out_attempted = true;
    }

    if (!adapter.beginConnect(ssid_buf_, password_buf_, channel_)) {
        if (snapshot_.connect_failures < 0xFFFFFFFFUL) {
            ++snapshot_.connect_failures;
        }
        enterBackoff(now_ms, WiFiError::BEGIN_CONNECT_FAILED);
        return;
    }

    attempt_started_ms_ = now_ms;
    snapshot_.state = WiFiState::CONNECTING;
    snapshot_.connected = false;
    snapshot_.has_valid_ip = false;
    snapshot_.rssi_dbm = 0;
    snapshot_.last_error = WiFiError::NONE;
}

void WiFiConnectionManager::enterBackoff(uint32_t now_ms, WiFiError error) {
    if (snapshot_.consecutive_failures == 0UL) {
        snapshot_.current_backoff_ms = backoff_initial_ms_;
    } else if (snapshot_.current_backoff_ms >= backoff_max_ms_ ||
               snapshot_.current_backoff_ms > (backoff_max_ms_ / 2UL)) {
        snapshot_.current_backoff_ms = backoff_max_ms_;
    } else {
        snapshot_.current_backoff_ms *= 2UL;
    }

    if (snapshot_.consecutive_failures < 0xFFFFFFFFUL) {
        ++snapshot_.consecutive_failures;
    }

    backoff_started_ms_ = now_ms;
    snapshot_.state = WiFiState::BACKOFF;
    snapshot_.connected = false;
    snapshot_.has_valid_ip = false;
    snapshot_.rssi_dbm = 0;
    snapshot_.last_error = error;
}

void WiFiConnectionManager::enterConnected() {
    snapshot_.state = WiFiState::CONNECTED;
    snapshot_.connected = true;
    snapshot_.has_valid_ip = true;
    snapshot_.last_error = WiFiError::NONE;
    snapshot_.consecutive_failures = 0UL;
    snapshot_.current_backoff_ms = backoff_initial_ms_;
    if (snapshot_.successful_connects < 0xFFFFFFFFUL) {
        ++snapshot_.successful_connects;
    }
}

bool WiFiConnectionManager::shouldEmitSerialLog(bool has_logged,
                                                uint32_t now_ms,
                                                uint32_t last_log_ms,
                                                bool state_changed,
                                                uint32_t steady_interval_ms,
                                                uint32_t min_change_interval_ms) {
    if (!has_logged) {
        return true;
    }
    const uint32_t elapsed_ms = static_cast<uint32_t>(now_ms - last_log_ms);
    if (state_changed && elapsed_ms >= min_change_interval_ms) {
        return true;
    }
    return elapsed_ms >= steady_interval_ms;
}

const char* WiFiConnectionManager::stateToString(WiFiState state) {
    switch (state) {
        case WiFiState::UNCONFIGURED:
            return "unconfigured";
        case WiFiState::CONFIG_ERROR:
            return "config_error";
        case WiFiState::DISCONNECTED:
            return "disconnected";
        case WiFiState::CONNECTING:
            return "connecting";
        case WiFiState::CONNECTED:
            return "connected";
        case WiFiState::BACKOFF:
            return "backoff";
        default:
            return "unknown";
    }
}

const char* WiFiConnectionManager::errorToString(WiFiError error) {
    switch (error) {
        case WiFiError::NONE:
            return "none";
        case WiFiError::UNCONFIGURED:
            return "unconfigured";
        case WiFiError::INVALID_CONFIG:
            return "invalid_config";
        case WiFiError::ADAPTER_INIT_FAILED:
            return "adapter_init_failed";
        case WiFiError::BEGIN_CONNECT_FAILED:
            return "begin_connect_failed";
        case WiFiError::CONNECT_TIMEOUT:
            return "connect_timeout";
        case WiFiError::LINK_FAILED:
            return "link_failed";
        case WiFiError::LINK_LOST:
            return "link_lost";
        case WiFiError::CONNECTED_WITHOUT_IP:
            return "connected_without_ip";
        default:
            return "unknown";
    }
}

}  // namespace node_sec
