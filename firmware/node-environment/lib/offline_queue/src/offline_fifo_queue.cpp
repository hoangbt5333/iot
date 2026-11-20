#include "offline_fifo_queue.h"

namespace node_env {

const char* queuePushStatusToString(QueuePushStatus status) {
    switch (status) {
        case QueuePushStatus::ENQUEUED:
            return "enqueued";
        case QueuePushStatus::ENQUEUED_DROPPED_OLDEST:
            return "enqueued_dropped_oldest";
        default:
            return "unknown";
    }
}

}  // namespace node_env
