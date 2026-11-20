#include "offline_security_event_queue.h"

namespace node_sec {

const char* securityEventPriorityToString(SecurityEventPriority priority) {
    switch (priority) {
        case SecurityEventPriority::LOW:
            return "low";
        case SecurityEventPriority::HIGH:
            return "high";
        default:
            return "unknown";
    }
}

const char* securityQueuePushStatusToString(SecurityQueuePushStatus status) {
    switch (status) {
        case SecurityQueuePushStatus::STORED:
            return "stored";
        case SecurityQueuePushStatus::EVICTED_OLD:
            return "evicted_old";
        case SecurityQueuePushStatus::REJECTED_INCOMING:
            return "rejected_incoming";
        default:
            return "unknown";
    }
}

}  // namespace node_sec
