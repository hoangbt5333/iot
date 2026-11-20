#pragma once

#include <cstddef>
#include <cstdint>

namespace node_sec {

enum class CommandAckStatus : uint8_t {
    NONE = 0,
    DUPLICATE = 1,
    EXPIRED = 2,
    REJECTED = 3
};

enum class CommandGuardDecision : uint8_t {
    ADMIT = 0,
    REJECT = 1
};

enum class CommandGuardReason : uint8_t {
    NONE = 0,
    INVALID_SCHEMA_VERSION = 1,
    INVALID_DEVICE_ID = 2,
    INVALID_COMMAND_ID = 3,
    ALREADY_PROCESSED = 4,
    INVALID_CLOCK = 5,
    INVALID_EXPIRY = 6,
    INVALID_TTL = 7,
    ISSUED_IN_FUTURE = 8,
    COMMAND_EXPIRED_UTC = 9,
    CLOCK_UNSYNCED_CANNOT_VERIFY_EXPIRY = 10,
    SESSION_NOT_READY = 11,
    BOOT_ID_MISMATCH = 12,
    COMMAND_EXPIRED_SESSION_TTL = 13
};

struct CommandGuardConfig {
    const char* expected_device_id = "node-b";
    size_t dedup_capacity = 24U;
    int64_t max_ttl_ms = 86400000LL;        // 24 hours upper bound to block corrupted TTLs
    int64_t max_future_skew_ms = 60000LL;   // 60s tolerance for issued_at vs now_utc
    bool allow_unsynced_session_ttl = false;  // PROP-02 remains unapproved by default
};

struct CommandEnvelope {
    int32_t schema_version = 0;
    const char* device_id = nullptr;
    const char* command_id = nullptr;
    bool has_issued_at = false;
    int64_t issued_at_epoch_ms = 0;
    bool has_expires_at = false;
    int64_t expires_at_epoch_ms = 0;
    bool has_ttl_ms = false;
    int64_t ttl_ms = 0;
    bool require_time_sync = true;
    const char* target_boot_id = nullptr;
    bool has_received_uptime_ms = false;
    uint64_t received_uptime_ms = 0;
};

struct ClockContext {
    bool time_synced = false;
    int64_t now_utc_epoch_ms = 0;
    uint64_t now_uptime_ms = 0;
    bool session_online = false;
    uint64_t session_online_uptime_ms = 0;
    const char* current_boot_id = nullptr;
};

struct AdmissionResult {
    CommandGuardDecision decision = CommandGuardDecision::REJECT;
    CommandAckStatus ack_status = CommandAckStatus::REJECTED;
    CommandGuardReason reason = CommandGuardReason::NONE;
};

// Volatile RAM command admission guard for Node B.
// Note: Deduplication ring-buffer lives strictly in RAM and is lost on reboot/power loss.
class CommandGuard {
public:
    static constexpr int32_t kExpectedSchemaVersion = 1;
    static constexpr size_t kMinCommandIdLen = 8U;
    static constexpr size_t kMaxCommandIdLen = 64U;
    static constexpr size_t kMaxDeviceIdLen = 32U;
    static constexpr size_t kMaxBootIdLen = 64U;
    static constexpr size_t kMaxDedupCapacity = 32U;
    static constexpr size_t kDefaultDedupCapacity = 24U;

    CommandGuard();
    explicit CommandGuard(const CommandGuardConfig& config);
    CommandGuard(const CommandGuard& other);
    CommandGuard& operator=(const CommandGuard& other);
    CommandGuard(CommandGuard&& other) noexcept;
    CommandGuard& operator=(CommandGuard&& other) noexcept;

    // Pure read-only admission check; does NOT mutate dedup history.
    AdmissionResult validate(const CommandEnvelope& cmd, const ClockContext& clock) const;

    // Must be called ONLY after downstream security logic actually applies the command.
    bool recordProcessed(const char* command_id);

    // Clears volatile RAM dedup history.
    void resetDedupHistory();

    bool isDuplicate(const char* command_id) const;
    size_t dedupCapacity() const { return active_capacity_; }
    size_t dedupCount() const { return count_; }
    const char* expectedDeviceId() const { return expected_device_id_; }
    const CommandGuardConfig& config() const { return config_; }

    static bool isValidCommandId(const char* command_id);
    static bool isValidSafeIdentifier(const char* text, size_t min_len, size_t max_len);
    static const char* reasonToString(CommandGuardReason reason);
    static const char* ackStatusToString(CommandAckStatus status);

private:
    struct DedupSlot {
        bool occupied = false;
        char command_id[kMaxCommandIdLen + 1U]{};
    };

    CommandGuardConfig config_{};
    char expected_device_id_[kMaxDeviceIdLen + 1U]{};
    DedupSlot ring_[kMaxDedupCapacity]{};
    size_t active_capacity_ = kDefaultDedupCapacity;
    size_t write_index_ = 0U;
    size_t count_ = 0U;

    void copyFrom(const CommandGuard& other);
    void sanitizeConfig(const CommandGuardConfig& raw_config);
    AdmissionResult validateUnsyncedSessionTtl(const CommandEnvelope& cmd,
                                               const ClockContext& clock) const;
    static AdmissionResult makeAdmit();
    static AdmissionResult makeReject(CommandAckStatus ack_status, CommandGuardReason reason);
};

}  // namespace node_sec
