#include "command_guard.h"

#include <cstring>

namespace node_sec {

namespace {

constexpr const char* kDefaultNodeBDeviceId = "node-b";

bool isAllowedIdChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

bool safeStrLenBounded(const char* text, size_t max_len, size_t* out_len) {
    if (text == nullptr || out_len == nullptr) {
        return false;
    }
    size_t len = 0U;
    while (len <= max_len && text[len] != '\0') {
        ++len;
    }
    if (len > max_len) {
        return false;
    }
    *out_len = len;
    return true;
}

bool safeStrEquals(const char* a, const char* b, size_t max_len) {
    size_t len_a = 0U;
    size_t len_b = 0U;
    if (!safeStrLenBounded(a, max_len, &len_a) || !safeStrLenBounded(b, max_len, &len_b)) {
        return false;
    }
    if (len_a != len_b) {
        return false;
    }
    return std::strncmp(a, b, len_a) == 0;
}

}  // namespace

CommandGuard::CommandGuard() : CommandGuard(CommandGuardConfig{}) {}

CommandGuard::CommandGuard(const CommandGuardConfig& config) {
    sanitizeConfig(config);
    resetDedupHistory();
}

CommandGuard::CommandGuard(const CommandGuard& other) {
    copyFrom(other);
}

CommandGuard& CommandGuard::operator=(const CommandGuard& other) {
    if (this != &other) {
        copyFrom(other);
    }
    return *this;
}

CommandGuard::CommandGuard(CommandGuard&& other) noexcept {
    copyFrom(other);
}

CommandGuard& CommandGuard::operator=(CommandGuard&& other) noexcept {
    if (this != &other) {
        copyFrom(other);
    }
    return *this;
}

void CommandGuard::copyFrom(const CommandGuard& other) {
    if (this == &other) {
        return;
    }
    config_ = other.config_;
    std::memcpy(expected_device_id_, other.expected_device_id_, sizeof(expected_device_id_));
    expected_device_id_[kMaxDeviceIdLen] = '\0';
    config_.expected_device_id = expected_device_id_;
    std::memcpy(ring_, other.ring_, sizeof(ring_));
    active_capacity_ = other.active_capacity_;
    write_index_ = other.write_index_;
    count_ = other.count_;
}

void CommandGuard::sanitizeConfig(const CommandGuardConfig& raw_config) {
    const char* source_id =
        isValidSafeIdentifier(raw_config.expected_device_id, 1U, kMaxDeviceIdLen)
            ? raw_config.expected_device_id
            : kDefaultNodeBDeviceId;

    size_t id_len = 0U;
    if (!safeStrLenBounded(source_id, kMaxDeviceIdLen, &id_len) || id_len > kMaxDeviceIdLen) {
        source_id = kDefaultNodeBDeviceId;
        id_len = std::strlen(kDefaultNodeBDeviceId);
    }
    std::memset(expected_device_id_, 0, sizeof(expected_device_id_));
    std::memcpy(expected_device_id_, source_id, id_len);
    expected_device_id_[id_len] = '\0';
    config_.expected_device_id = expected_device_id_;

    if (raw_config.dedup_capacity >= 1U && raw_config.dedup_capacity <= kMaxDedupCapacity) {
        config_.dedup_capacity = raw_config.dedup_capacity;
    } else {
        config_.dedup_capacity = kDefaultDedupCapacity;
    }
    active_capacity_ = config_.dedup_capacity;

    config_.max_ttl_ms = (raw_config.max_ttl_ms > 0LL) ? raw_config.max_ttl_ms : 86400000LL;
    config_.max_future_skew_ms =
        (raw_config.max_future_skew_ms >= 0LL) ? raw_config.max_future_skew_ms : 60000LL;
    config_.allow_unsynced_session_ttl = raw_config.allow_unsynced_session_ttl;
}

AdmissionResult CommandGuard::validate(const CommandEnvelope& cmd, const ClockContext& clock) const {
    if (cmd.schema_version != kExpectedSchemaVersion) {
        return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::INVALID_SCHEMA_VERSION);
    }

    if (!safeStrEquals(cmd.device_id, expected_device_id_, kMaxDeviceIdLen)) {
        return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::INVALID_DEVICE_ID);
    }

    if (!isValidCommandId(cmd.command_id)) {
        return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::INVALID_COMMAND_ID);
    }

    if (isDuplicate(cmd.command_id)) {
        return makeReject(CommandAckStatus::DUPLICATE, CommandGuardReason::ALREADY_PROCESSED);
    }

    if (!cmd.has_expires_at || cmd.expires_at_epoch_ms <= 0LL) {
        return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::INVALID_EXPIRY);
    }

    if (cmd.has_issued_at) {
        if (cmd.issued_at_epoch_ms <= 0LL || cmd.issued_at_epoch_ms >= cmd.expires_at_epoch_ms) {
            return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::INVALID_EXPIRY);
        }
    }

    if (cmd.has_ttl_ms) {
        if (cmd.ttl_ms <= 0LL || cmd.ttl_ms > config_.max_ttl_ms) {
            return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::INVALID_TTL);
        }
    }

    if (!clock.time_synced) {
        if (cmd.require_time_sync || !config_.allow_unsynced_session_ttl) {
            return makeReject(CommandAckStatus::EXPIRED,
                              CommandGuardReason::CLOCK_UNSYNCED_CANNOT_VERIFY_EXPIRY);
        }
        return validateUnsyncedSessionTtl(cmd, clock);
    }

    if (clock.now_utc_epoch_ms <= 0LL) {
        return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::INVALID_CLOCK);
    }

    if (cmd.has_issued_at) {
        // Overflow-safe check: issued_at > 0, now_utc > 0, and issued_at > now_utc here.
        if (cmd.issued_at_epoch_ms > clock.now_utc_epoch_ms &&
            (cmd.issued_at_epoch_ms - clock.now_utc_epoch_ms) > config_.max_future_skew_ms) {
            return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::ISSUED_IN_FUTURE);
        }
    }

    if (clock.now_utc_epoch_ms > cmd.expires_at_epoch_ms) {
        return makeReject(CommandAckStatus::EXPIRED, CommandGuardReason::COMMAND_EXPIRED_UTC);
    }

    return makeAdmit();
}

AdmissionResult CommandGuard::validateUnsyncedSessionTtl(const CommandEnvelope& cmd,
                                                         const ClockContext& clock) const {
    if (!clock.session_online ||
        !isValidSafeIdentifier(clock.current_boot_id, 1U, kMaxBootIdLen)) {
        return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::SESSION_NOT_READY);
    }

    if (!safeStrEquals(cmd.target_boot_id, clock.current_boot_id, kMaxBootIdLen)) {
        return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::BOOT_ID_MISMATCH);
    }

    if (!cmd.has_ttl_ms || cmd.ttl_ms <= 0LL || cmd.ttl_ms > config_.max_ttl_ms) {
        return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::INVALID_TTL);
    }

    if (!cmd.has_received_uptime_ms ||
        cmd.received_uptime_ms < clock.session_online_uptime_ms ||
        clock.now_uptime_ms < cmd.received_uptime_ms) {
        return makeReject(CommandAckStatus::REJECTED, CommandGuardReason::INVALID_CLOCK);
    }

    const uint64_t elapsed_ms = clock.now_uptime_ms - cmd.received_uptime_ms;
    if (elapsed_ms > static_cast<uint64_t>(cmd.ttl_ms)) {
        return makeReject(CommandAckStatus::EXPIRED,
                          CommandGuardReason::COMMAND_EXPIRED_SESSION_TTL);
    }

    return makeAdmit();
}

bool CommandGuard::recordProcessed(const char* command_id) {
    if (!isValidCommandId(command_id)) {
        return false;
    }
    // Keep existing entry untouched so QoS 1 retries do not evict older distinct IDs.
    if (isDuplicate(command_id)) {
        return true;
    }

    size_t len = 0U;
    safeStrLenBounded(command_id, kMaxCommandIdLen, &len);

    DedupSlot& slot = ring_[write_index_];
    slot.occupied = true;
    std::memcpy(slot.command_id, command_id, len);
    slot.command_id[len] = '\0';

    write_index_ = (write_index_ + 1U) % active_capacity_;
    if (count_ < active_capacity_) {
        ++count_;
    }
    return true;
}

void CommandGuard::resetDedupHistory() {
    for (size_t i = 0U; i < kMaxDedupCapacity; ++i) {
        ring_[i].occupied = false;
        ring_[i].command_id[0] = '\0';
    }
    write_index_ = 0U;
    count_ = 0U;
}

bool CommandGuard::isDuplicate(const char* command_id) const {
    if (!isValidCommandId(command_id)) {
        return false;
    }
    for (size_t i = 0U; i < active_capacity_; ++i) {
        if (ring_[i].occupied &&
            safeStrEquals(ring_[i].command_id, command_id, kMaxCommandIdLen)) {
            return true;
        }
    }
    return false;
}

bool CommandGuard::isValidCommandId(const char* command_id) {
    return isValidSafeIdentifier(command_id, kMinCommandIdLen, kMaxCommandIdLen);
}

bool CommandGuard::isValidSafeIdentifier(const char* text, size_t min_len, size_t max_len) {
    size_t len = 0U;
    if (!safeStrLenBounded(text, max_len, &len)) {
        return false;
    }
    if (len < min_len || len > max_len) {
        return false;
    }
    for (size_t i = 0U; i < len; ++i) {
        if (!isAllowedIdChar(text[i])) {
            return false;
        }
    }
    return true;
}

const char* CommandGuard::reasonToString(CommandGuardReason reason) {
    switch (reason) {
        case CommandGuardReason::NONE:
            return "none";
        case CommandGuardReason::INVALID_SCHEMA_VERSION:
            return "invalid_schema_version";
        case CommandGuardReason::INVALID_DEVICE_ID:
            return "invalid_device_id";
        case CommandGuardReason::INVALID_COMMAND_ID:
            return "invalid_command_id";
        case CommandGuardReason::ALREADY_PROCESSED:
            return "already_processed";
        case CommandGuardReason::INVALID_CLOCK:
            return "invalid_clock";
        case CommandGuardReason::INVALID_EXPIRY:
            return "invalid_expiry";
        case CommandGuardReason::INVALID_TTL:
            return "invalid_ttl";
        case CommandGuardReason::ISSUED_IN_FUTURE:
            return "issued_in_future";
        case CommandGuardReason::COMMAND_EXPIRED_UTC:
            return "command_expired_utc";
        case CommandGuardReason::CLOCK_UNSYNCED_CANNOT_VERIFY_EXPIRY:
            return "clock_unsynced_cannot_verify_expiry";
        case CommandGuardReason::SESSION_NOT_READY:
            return "session_not_ready";
        case CommandGuardReason::BOOT_ID_MISMATCH:
            return "boot_id_mismatch";
        case CommandGuardReason::COMMAND_EXPIRED_SESSION_TTL:
            return "command_expired_session_ttl";
        default:
            return "unknown";
    }
}

const char* CommandGuard::ackStatusToString(CommandAckStatus status) {
    switch (status) {
        case CommandAckStatus::NONE:
            return "none";
        case CommandAckStatus::DUPLICATE:
            return "duplicate";
        case CommandAckStatus::EXPIRED:
            return "expired";
        case CommandAckStatus::REJECTED:
        default:
            return "rejected";
    }
}

AdmissionResult CommandGuard::makeAdmit() {
    AdmissionResult res;
    res.decision = CommandGuardDecision::ADMIT;
    res.ack_status = CommandAckStatus::NONE;
    res.reason = CommandGuardReason::NONE;
    return res;
}

AdmissionResult CommandGuard::makeReject(CommandAckStatus ack_status, CommandGuardReason reason) {
    AdmissionResult res;
    res.decision = CommandGuardDecision::REJECT;
    res.ack_status = ack_status;
    res.reason = reason;
    return res;
}

}  // namespace node_sec
