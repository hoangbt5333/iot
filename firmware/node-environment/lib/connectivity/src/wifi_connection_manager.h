#pragma once

#include <cstddef>
#include <cstdint>

namespace node_env {

enum class WiFiState : uint8_t {
    UNCONFIGURED = 0,
    CONFIG_ERROR = 1,
    DISCONNECTED = 2,
    CONNECTING = 3,
    CONNECTED = 4,
    BACKOFF = 5
};

enum class WiFiLinkStatus : uint8_t {
    IDLE = 0,
    NO_SSID_AVAIL = 1,
    CONNECTED = 2,
    CONNECT_FAILED = 3,
    CONNECTION_LOST = 4,
    DISCONNECTED = 5,
    STOPPED = 6,
    HARDWARE_UNAVAILABLE = 7
};

enum class WiFiError : uint8_t {
    NONE = 0,
    UNCONFIGURED = 1,
    INVALID_CONFIG = 2,
    ADAPTER_INIT_FAILED = 3,
    BEGIN_CONNECT_FAILED = 4,
    CONNECT_TIMEOUT = 5,
    LINK_FAILED = 6,
    LINK_LOST = 7,
    CONNECTED_WITHOUT_IP = 8
};

struct WiFiStationConfig {
    bool enabled = false;
    const char* ssid = nullptr;
    const char* password = nullptr;
    int32_t channel = 0;
    uint32_t connect_timeout_ms = 10000UL;
    uint32_t backoff_initial_ms = 1000UL;
    uint32_t backoff_max_ms = 15000UL;
};

struct WiFiAdapterStatus {
    bool adapter_ok = true;
    WiFiLinkStatus link_status = WiFiLinkStatus::DISCONNECTED;
    bool has_valid_ip = false;
    int8_t rssi_dbm = 0;
};

struct WiFiStatusSnapshot {
    WiFiState state = WiFiState::UNCONFIGURED;
    WiFiError last_error = WiFiError::UNCONFIGURED;
    bool configured = false;
    bool connected = false;
    bool has_valid_ip = false;
    int8_t rssi_dbm = 0;
    uint32_t current_backoff_ms = 0UL;
    uint32_t connect_attempts = 0UL;
    uint32_t connect_failures = 0UL;
    uint32_t successful_connects = 0UL;
    uint32_t disconnect_count = 0UL;
    uint32_t consecutive_failures = 0UL;
};

struct WiFiStepOutcome {
    WiFiState previous_state = WiFiState::UNCONFIGURED;
    WiFiState current_state = WiFiState::UNCONFIGURED;
    bool state_changed = false;
    bool connect_attempted = false;
    WiFiStatusSnapshot snapshot{};
};

class IWiFiAdapter {
public:
    virtual ~IWiFiAdapter() = default;
    virtual bool configureStation() = 0;
    virtual bool beginConnect(const char* ssid, const char* password, int32_t channel) = 0;
    virtual void disconnectAttempt() = 0;
    virtual WiFiAdapterStatus pollStatus() = 0;
};

// Pure C++17 non-blocking Wi-Fi Station state machine with rollover-safe timing and capped backoff.
class WiFiConnectionManager {
public:
    static constexpr size_t kMaxSsidLen = 32U;
    static constexpr size_t kMaxPasswordLen = 64U;
    static constexpr int32_t kMinChannel = 0;
    static constexpr int32_t kMaxChannel = 14;
    static constexpr uint32_t kDefaultConnectTimeoutMs = 10000UL;
    static constexpr uint32_t kDefaultBackoffInitialMs = 1000UL;
    static constexpr uint32_t kDefaultBackoffMaxMs = 15000UL;
    static constexpr uint32_t kMinConnectTimeoutMs = 50UL;
    static constexpr uint32_t kMinBackoffMs = 50UL;

    WiFiConnectionManager();
    explicit WiFiConnectionManager(const WiFiStationConfig& config);

    void configure(const WiFiStationConfig& config);
    void begin(IWiFiAdapter& adapter);
    void requestDisconnect(IWiFiAdapter& adapter);
    WiFiStepOutcome tick(uint32_t now_ms, IWiFiAdapter& adapter);

    WiFiState state() const { return snapshot_.state; }
    WiFiError lastError() const { return snapshot_.last_error; }
    bool isConfigured() const { return snapshot_.configured; }
    bool isConnected() const { return snapshot_.connected; }
    uint32_t currentBackoffMs() const { return snapshot_.current_backoff_ms; }
    const WiFiStatusSnapshot& snapshot() const { return snapshot_; }

    static bool shouldEmitSerialLog(bool has_logged,
                                    uint32_t now_ms,
                                    uint32_t last_log_ms,
                                    bool state_changed,
                                    uint32_t steady_interval_ms,
                                    uint32_t min_change_interval_ms = 1000UL);
    static const char* stateToString(WiFiState state);
    static const char* errorToString(WiFiError error);

private:
    char ssid_buf_[kMaxSsidLen + 1U]{};
    char password_buf_[kMaxPasswordLen + 1U]{};
    int32_t channel_ = 0;
    uint32_t connect_timeout_ms_ = kDefaultConnectTimeoutMs;
    uint32_t backoff_initial_ms_ = kDefaultBackoffInitialMs;
    uint32_t backoff_max_ms_ = kDefaultBackoffMaxMs;

    bool station_configured_ = false;
    uint32_t attempt_started_ms_ = 0UL;
    uint32_t backoff_started_ms_ = 0UL;
    WiFiStatusSnapshot snapshot_{};

    void sanitizeAndStoreConfig(const WiFiStationConfig& raw);
    void startConnectAttempt(uint32_t now_ms, IWiFiAdapter& adapter, bool* out_attempted);
    void enterBackoff(uint32_t now_ms, WiFiError error);
    void enterConnected();
};

}  // namespace node_env
