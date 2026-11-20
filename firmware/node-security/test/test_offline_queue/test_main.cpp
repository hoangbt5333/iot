#include <unity.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "node_security.h"
#include "offline_security_event_queue.h"

using node_sec::BoundedSecurityEventQueue;
using node_sec::SecurityEventPriority;
using node_sec::SecurityQueuePushStatus;
using node_sec::SecurityState;
using node_sec::is_valid_security_queue_payload;
using node_sec::is_valid_security_queue_payload_v;
using node_sec::securityEventPriorityToString;
using node_sec::securityQueuePushStatusToString;

namespace {

// Pure C-style POD (trivially default-constructible, copyable, and destructible).
struct PureTrivialSecurityPod {
    uint64_t uptime_ms;
    uint32_t event_id;
    float metric;
    bool valid;
};

// Test-only POD security event snapshot (NOT a frozen ThingsBoard or contract wire schema).
struct TestSecurityEventPod {
    uint32_t event_id = 0U;
    uint64_t uptime_ms = 0ULL;
    bool valid = false;
    float aux_value = 0.0f;
    uint8_t event_kind = 0U;
    char note[32]{};
};

// Custom allocating-copy type (MUST be rejected at compile time).
struct CustomHeapCopyEvent {
    char* buffer = nullptr;
    size_t len = 0U;

    CustomHeapCopyEvent() = default;
    CustomHeapCopyEvent(const CustomHeapCopyEvent& other) : len(other.len) {
        if (other.buffer != nullptr) {
            buffer = new char[len + 1U];
            std::memcpy(buffer, other.buffer, len + 1U);
        }
    }
    CustomHeapCopyEvent& operator=(const CustomHeapCopyEvent& other) {
        if (this != &other) {
            delete[] buffer;
            buffer = nullptr;
            len = other.len;
            if (other.buffer != nullptr) {
                buffer = new char[len + 1U];
                std::memcpy(buffer, other.buffer, len + 1U);
            }
        }
        return *this;
    }
    ~CustomHeapCopyEvent() { delete[] buffer; }
};

struct CustomCopyAssignOnlyEvent {
    int32_t code = 0;
    CustomCopyAssignOnlyEvent() = default;
    CustomCopyAssignOnlyEvent(const CustomCopyAssignOnlyEvent&) = default;
    CustomCopyAssignOnlyEvent& operator=(const CustomCopyAssignOnlyEvent& other) {
        code = other.code;
        return *this;
    }
    ~CustomCopyAssignOnlyEvent() = default;
};

struct StructWithStdStringEvent {
    uint32_t event_id = 0U;
    std::string detail;
};

// Compile-time payload acceptance and rejection assertions.
static_assert(is_valid_security_queue_payload<PureTrivialSecurityPod>::value,
              "PureTrivialSecurityPod must be accepted.");
static_assert(is_valid_security_queue_payload<TestSecurityEventPod>::value,
              "TestSecurityEventPod must be accepted.");
static_assert(is_valid_security_queue_payload<SecurityState>::value,
              "SecurityState must be accepted.");
static_assert(is_valid_security_queue_payload_v<uint32_t>, "uint32_t must be accepted.");

static_assert(!is_valid_security_queue_payload<std::string>::value,
              "std::string must be rejected.");
static_assert(!is_valid_security_queue_payload<std::vector<uint8_t>>::value,
              "std::vector must be rejected.");
static_assert(!is_valid_security_queue_payload<CustomHeapCopyEvent>::value,
              "CustomHeapCopyEvent must be rejected.");
static_assert(!is_valid_security_queue_payload<CustomCopyAssignOnlyEvent>::value,
              "CustomCopyAssignOnlyEvent must be rejected.");
static_assert(!is_valid_security_queue_payload<StructWithStdStringEvent>::value,
              "StructWithStdStringEvent must be rejected.");
static_assert(!is_valid_security_queue_payload<int*>::value, "Raw pointer int* must be rejected.");
static_assert(!is_valid_security_queue_payload<const char*>::value,
              "Raw pointer const char* must be rejected.");
static_assert(!is_valid_security_queue_payload<TestSecurityEventPod&>::value,
              "Reference type must be rejected.");
static_assert(!is_valid_security_queue_payload<const TestSecurityEventPod>::value,
              "const-qualified type must be rejected.");
static_assert(!is_valid_security_queue_payload<char[16]>::value, "Raw array must be rejected.");
static_assert(!is_valid_security_queue_payload<void>::value, "void must be rejected.");

TestSecurityEventPod makeEvent(uint32_t event_id,
                               uint64_t uptime_ms,
                               bool valid = true,
                               const char* note = "evt") {
    TestSecurityEventPod ev{};
    ev.event_id = event_id;
    ev.uptime_ms = uptime_ms;
    ev.valid = valid;
    ev.aux_value = valid ? 1.0f : std::numeric_limits<float>::quiet_NaN();
    ev.event_kind = static_cast<uint8_t>(event_id & 0xFFU);
    if (note != nullptr) {
        const size_t len = std::strlen(note);
        const size_t copy_len = (len < sizeof(ev.note) - 1U) ? len : (sizeof(ev.note) - 1U);
        if (copy_len > 0U) {
            std::memcpy(ev.note, note, copy_len);
        }
        ev.note[copy_len] = '\0';
    }
    return ev;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_capacity_one_priority_evict_and_reject(void) {
    BoundedSecurityEventQueue<TestSecurityEventPod, 1U> queue;

    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(queue.capacity()));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_FALSE(queue.full());
    TEST_ASSERT_EQUAL_UINT64(0ULL, queue.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(0ULL, queue.rejected_count());
    TEST_ASSERT_EQUAL_UINT64(0ULL, queue.total_dropped_count());
    TEST_ASSERT_NULL(queue.peek());
    TEST_ASSERT_NULL(queue.front());
    TEST_ASSERT_FALSE(queue.pop());

    TestSecurityEventPod out = makeEvent(999U, 999ULL);
    TEST_ASSERT_FALSE(queue.peek(&out));
    TEST_ASSERT_EQUAL_UINT32(999U, out.event_id);
    TEST_ASSERT_FALSE(queue.pop(&out));

    // Store initial LOW event.
    const auto r1 = queue.push(makeEvent(1U, 100ULL), SecurityEventPriority::LOW);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityQueuePushStatus::STORED),
                            static_cast<uint8_t>(r1.status));
    TEST_ASSERT_EQUAL_STRING("stored", securityQueuePushStatusToString(r1.status));
    TEST_ASSERT_TRUE(r1.stored);
    TEST_ASSERT_FALSE(r1.evicted_old);
    TEST_ASSERT_FALSE(r1.rejected);
    TEST_ASSERT_TRUE(queue.full());
    TEST_ASSERT_EQUAL_UINT32(1U, queue.peek()->event_id);

    // LOW replaces LOW when full (same priority -> evict oldest).
    const auto r2 = queue.push(makeEvent(2U, 200ULL), SecurityEventPriority::LOW);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityQueuePushStatus::EVICTED_OLD),
                            static_cast<uint8_t>(r2.status));
    TEST_ASSERT_EQUAL_STRING("evicted_old", securityQueuePushStatusToString(r2.status));
    TEST_ASSERT_TRUE(r2.stored);
    TEST_ASSERT_TRUE(r2.evicted_old);
    TEST_ASSERT_FALSE(r2.rejected);
    TEST_ASSERT_EQUAL_UINT64(1ULL, queue.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(0ULL, queue.rejected_count());
    TEST_ASSERT_EQUAL_UINT32(2U, queue.peek()->event_id);

    // HIGH replaces LOW when full.
    const auto r3 = queue.push(makeEvent(3U, 300ULL), SecurityEventPriority::HIGH);
    TEST_ASSERT_TRUE(r3.evicted_old);
    TEST_ASSERT_EQUAL_UINT64(2ULL, queue.evicted_count());
    TEST_ASSERT_EQUAL_UINT32(3U, queue.peek()->event_id);

    // LOW rejected when HIGH is stored.
    const auto r4 = queue.push(makeEvent(4U, 400ULL), SecurityEventPriority::LOW);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityQueuePushStatus::REJECTED_INCOMING),
                            static_cast<uint8_t>(r4.status));
    TEST_ASSERT_EQUAL_STRING("rejected_incoming", securityQueuePushStatusToString(r4.status));
    TEST_ASSERT_FALSE(r4.stored);
    TEST_ASSERT_FALSE(r4.evicted_old);
    TEST_ASSERT_TRUE(r4.rejected);
    TEST_ASSERT_EQUAL_UINT64(2ULL, queue.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(1ULL, queue.rejected_count());
    TEST_ASSERT_EQUAL_UINT64(3ULL, queue.total_dropped_count());
    TEST_ASSERT_EQUAL_UINT32(3U, queue.peek()->event_id);

    // HIGH replaces HIGH when full.
    const auto r5 = queue.push(makeEvent(5U, 500ULL), SecurityEventPriority::HIGH);
    TEST_ASSERT_TRUE(r5.evicted_old);
    TEST_ASSERT_EQUAL_UINT64(3ULL, queue.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(1ULL, queue.rejected_count());
    TEST_ASSERT_EQUAL_UINT64(4ULL, queue.total_dropped_count());

    SecurityEventPriority popped_prio = SecurityEventPriority::LOW;
    TEST_ASSERT_TRUE(queue.pop(&out, &popped_prio));
    TEST_ASSERT_EQUAL_UINT32(5U, out.event_id);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityEventPriority::HIGH),
                            static_cast<uint8_t>(popped_prio));
    TEST_ASSERT_TRUE(queue.empty());
}

void test_capacity_20_default_fifo_and_all_same_priority_overflow(void) {
    // Default Capacity template argument must be 20.
    BoundedSecurityEventQueue<TestSecurityEventPod> default_q;
    TEST_ASSERT_EQUAL_UINT32(20U, static_cast<uint32_t>(default_q.capacity()));
    TEST_ASSERT_EQUAL_STRING("low", securityEventPriorityToString(SecurityEventPriority::LOW));
    TEST_ASSERT_EQUAL_STRING("high", securityEventPriorityToString(SecurityEventPriority::HIGH));

    // All-LOW fill 20 + overflow 5 -> oldest 1..5 evicted, 6..25 remain FIFO.
    for (uint32_t i = 1U; i <= 20U; ++i) {
        const auto res = default_q.push(makeEvent(i, i * 10ULL), SecurityEventPriority::LOW);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityQueuePushStatus::STORED),
                                static_cast<uint8_t>(res.status));
    }
    TEST_ASSERT_TRUE(default_q.full());
    for (uint32_t i = 21U; i <= 25U; ++i) {
        const auto res = default_q.push(makeEvent(i, i * 10ULL), SecurityEventPriority::LOW);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityQueuePushStatus::EVICTED_OLD),
                                static_cast<uint8_t>(res.status));
    }
    TEST_ASSERT_EQUAL_UINT64(5ULL, default_q.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(0ULL, default_q.rejected_count());

    for (uint32_t expected = 6U; expected <= 25U; ++expected) {
        TestSecurityEventPod out{};
        TEST_ASSERT_TRUE(default_q.pop(&out));
        TEST_ASSERT_EQUAL_UINT32(expected, out.event_id);
    }
    TEST_ASSERT_TRUE(default_q.empty());

    // All-HIGH fill 20: LOW incoming rejected, HIGH incoming evicts oldest HIGH.
    BoundedSecurityEventQueue<TestSecurityEventPod, 20U> high_q;
    for (uint32_t i = 101U; i <= 120U; ++i) {
        high_q.push(makeEvent(i, i * 10ULL), SecurityEventPriority::HIGH);
    }
    for (uint32_t low_id = 200U; low_id < 205U; ++low_id) {
        const auto rej = high_q.push(makeEvent(low_id, low_id), SecurityEventPriority::LOW);
        TEST_ASSERT_TRUE(rej.rejected);
        TEST_ASSERT_FALSE(rej.stored);
    }
    TEST_ASSERT_EQUAL_UINT64(0ULL, high_q.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(5ULL, high_q.rejected_count());
    TEST_ASSERT_EQUAL_UINT32(101U, high_q.peek()->event_id);

    const auto ev = high_q.push(makeEvent(301U, 3010ULL), SecurityEventPriority::HIGH);
    TEST_ASSERT_TRUE(ev.evicted_old);
    TEST_ASSERT_EQUAL_UINT64(1ULL, high_q.evicted_count());
    TEST_ASSERT_EQUAL_UINT32(102U, high_q.peek()->event_id);
}

void test_mixed_priority_middle_eviction_and_fifo_order_with_wraparound(void) {
    // Advance head_ to non-zero offset first to exercise ring wrap during middle eviction.
    BoundedSecurityEventQueue<TestSecurityEventPod, 5U> q;
    q.push(makeEvent(1U, 10ULL), SecurityEventPriority::LOW);
    q.push(makeEvent(2U, 20ULL), SecurityEventPriority::LOW);
    q.push(makeEvent(3U, 30ULL), SecurityEventPriority::LOW);
    TEST_ASSERT_TRUE(q.pop());
    TEST_ASSERT_TRUE(q.pop());
    TEST_ASSERT_TRUE(q.pop());  // Now empty with head_ == 3, tail_ == 3!

    // Populate across wrap boundary: [H10, L20, H30, L40, H50].
    q.push(makeEvent(10U, 100ULL), SecurityEventPriority::HIGH);
    q.push(makeEvent(20U, 200ULL), SecurityEventPriority::LOW);
    q.push(makeEvent(30U, 300ULL), SecurityEventPriority::HIGH);
    q.push(makeEvent(40U, 400ULL), SecurityEventPriority::LOW);
    q.push(makeEvent(50U, 500ULL), SecurityEventPriority::HIGH);
    TEST_ASSERT_TRUE(q.full());

    // Push HIGH(60): must evict oldest LOW in the middle (L20), keeping [H10, H30, L40, H50, H60].
    const auto r1 = q.push(makeEvent(60U, 600ULL), SecurityEventPriority::HIGH);
    TEST_ASSERT_TRUE(r1.evicted_old);
    TEST_ASSERT_EQUAL_UINT64(1ULL, q.evicted_count());

    // Push LOW(70): must evict remaining oldest LOW in the middle (L40), keeping all HIGHs:
    // [H10, H30, H50, H60, L70].
    const auto r2 = q.push(makeEvent(70U, 700ULL), SecurityEventPriority::LOW);
    TEST_ASSERT_TRUE(r2.evicted_old);
    TEST_ASSERT_EQUAL_UINT64(2ULL, q.evicted_count());

    // Push HIGH(80): evicts L70 (at tail), leaving all 5 HIGH: [H10, H30, H50, H60, H80].
    const auto r3 = q.push(makeEvent(80U, 800ULL), SecurityEventPriority::HIGH);
    TEST_ASSERT_TRUE(r3.evicted_old);

    // Push LOW(90): all stored are HIGH -> REJECTED_INCOMING!
    const auto r4 = q.push(makeEvent(90U, 900ULL), SecurityEventPriority::LOW);
    TEST_ASSERT_TRUE(r4.rejected);
    TEST_ASSERT_EQUAL_UINT64(3ULL, q.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(1ULL, q.rejected_count());

    const uint32_t expected_ids[5] = {10U, 30U, 50U, 60U, 80U};
    for (size_t i = 0U; i < 5U; ++i) {
        TestSecurityEventPod out{};
        SecurityEventPriority prio = SecurityEventPriority::LOW;
        TEST_ASSERT_TRUE(q.pop(&out, &prio));
        TEST_ASSERT_EQUAL_UINT32(expected_ids[i], out.event_id);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityEventPriority::HIGH),
                                static_cast<uint8_t>(prio));
    }
    TEST_ASSERT_TRUE(q.empty());
}

void test_wrap_around_and_sequence_ordering_across_many_evictions(void) {
    BoundedSecurityEventQueue<TestSecurityEventPod, 20U> q;
    uint32_t next_id = 1U;

    // Interleave pushes and partial pops across 25 cycles (multiple full ring wrap-arounds).
    uint32_t prev_popped = 0U;
    for (int cycle = 0; cycle < 25; ++cycle) {
        for (int i = 0; i < 15; ++i) {
            const SecurityEventPriority prio =
                (next_id % 4U == 0U) ? SecurityEventPriority::HIGH : SecurityEventPriority::LOW;
            q.push(makeEvent(next_id, next_id * 10ULL), prio);
            ++next_id;
        }
        for (int i = 0; i < 7; ++i) {
            if (q.empty()) {
                break;
            }
            TestSecurityEventPod out{};
            TEST_ASSERT_TRUE(q.pop(&out));
            // Retained elements MUST strictly preserve arrival order (monotonic event_id).
            TEST_ASSERT_TRUE(out.event_id > prev_popped);
            prev_popped = out.event_id;
        }
    }

    while (!q.empty()) {
        TestSecurityEventPod out{};
        TEST_ASSERT_TRUE(q.pop(&out));
        TEST_ASSERT_TRUE(out.event_id > prev_popped);
        prev_popped = out.event_id;
    }
    TEST_ASSERT_TRUE(q.evicted_count() > 0ULL);
    TEST_ASSERT_EQUAL_UINT64(0ULL, q.rejected_count());
}

void test_counter_saturation_for_evicted_rejected_and_total_dropped(void) {
    // 1. Saturate evicted_count at 255 using uint8_t CounterT.
    BoundedSecurityEventQueue<TestSecurityEventPod, 2U, uint8_t> evict_q;
    evict_q.push(makeEvent(1U, 10ULL), SecurityEventPriority::LOW);
    evict_q.push(makeEvent(2U, 20ULL), SecurityEventPriority::LOW);
    for (uint32_t i = 3U; i <= 262U; ++i) {
        const auto res = evict_q.push(makeEvent(i, i), SecurityEventPriority::LOW);
        TEST_ASSERT_TRUE(res.evicted_old);
        const uint8_t expected = (i - 2U >= 255U) ? 255U : static_cast<uint8_t>(i - 2U);
        TEST_ASSERT_EQUAL_UINT8(expected, evict_q.evicted_count());
        TEST_ASSERT_EQUAL_UINT8(expected, res.evicted_count_after);
    }
    TEST_ASSERT_EQUAL_UINT8(255U, evict_q.evicted_count());
    TEST_ASSERT_EQUAL_UINT8(255U, evict_q.total_dropped_count());

    // 2. Saturate rejected_count at 255 and test saturated sum when both are 150 / 255.
    BoundedSecurityEventQueue<TestSecurityEventPod, 2U, uint8_t> sat_sum_q;
    sat_sum_q.push(makeEvent(1U, 1ULL), SecurityEventPriority::HIGH);
    sat_sum_q.push(makeEvent(2U, 2ULL), SecurityEventPriority::HIGH);

    for (uint32_t i = 0U; i < 150U; ++i) {
        const auto r = sat_sum_q.push(makeEvent(100U + i, 100U + i), SecurityEventPriority::HIGH);
        TEST_ASSERT_TRUE(r.evicted_old);
    }
    TEST_ASSERT_EQUAL_UINT8(150U, sat_sum_q.evicted_count());

    for (uint32_t i = 0U; i < 260U; ++i) {
        const auto r = sat_sum_q.push(makeEvent(500U + i, 500U + i), SecurityEventPriority::LOW);
        TEST_ASSERT_TRUE(r.rejected);
    }
    TEST_ASSERT_EQUAL_UINT8(150U, sat_sum_q.evicted_count());
    TEST_ASSERT_EQUAL_UINT8(255U, sat_sum_q.rejected_count());
    // 150 + 255 > 255 -> total_dropped_count() MUST saturate at 255, never wrap.
    TEST_ASSERT_EQUAL_UINT8(255U, sat_sum_q.total_dropped_count());

    // Verify default CounterT is uint64_t.
    BoundedSecurityEventQueue<TestSecurityEventPod> default_q;
    const bool is_u64 = std::is_same<decltype(default_q.evicted_count()), uint64_t>::value &&
                        std::is_same<decltype(default_q.rejected_count()), uint64_t>::value &&
                        std::is_same<decltype(default_q.total_dropped_count()), uint64_t>::value;
    TEST_ASSERT_TRUE(is_u64);
}

void test_clear_preserves_counters_and_reset_counters_clears_stats(void) {
    BoundedSecurityEventQueue<TestSecurityEventPod, 2U> q;
    q.push(makeEvent(1U, 10ULL), SecurityEventPriority::HIGH);
    q.push(makeEvent(2U, 20ULL), SecurityEventPriority::HIGH);
    q.push(makeEvent(3U, 30ULL), SecurityEventPriority::HIGH);  // evicted=1
    q.push(makeEvent(4U, 40ULL), SecurityEventPriority::LOW);   // rejected=1

    TEST_ASSERT_EQUAL_UINT64(1ULL, q.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(1ULL, q.rejected_count());
    TEST_ASSERT_EQUAL_UINT64(2ULL, q.total_dropped_count());

    q.clear();
    TEST_ASSERT_TRUE(q.empty());
    TEST_ASSERT_FALSE(q.full());
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(q.size()));
    TEST_ASSERT_NULL(q.peek());
    TEST_ASSERT_FALSE(q.pop());
    // Clear MUST preserve counters.
    TEST_ASSERT_EQUAL_UINT64(1ULL, q.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(1ULL, q.rejected_count());
    TEST_ASSERT_EQUAL_UINT64(2ULL, q.total_dropped_count());

    // Store new event after clear, then explicitly reset counters.
    q.push(makeEvent(5U, 50ULL), SecurityEventPriority::LOW);
    q.reset_counters();
    TEST_ASSERT_EQUAL_UINT64(0ULL, q.evicted_count());
    TEST_ASSERT_EQUAL_UINT64(0ULL, q.rejected_count());
    TEST_ASSERT_EQUAL_UINT64(0ULL, q.total_dropped_count());
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(q.size()));
    TEST_ASSERT_EQUAL_UINT32(5U, q.peek()->event_id);
}

void test_peek_non_consuming_failed_send_retains_and_ack_pops_one(void) {
    BoundedSecurityEventQueue<TestSecurityEventPod, 20U> q;
    q.push(makeEvent(11U, 100ULL), SecurityEventPriority::HIGH);
    q.push(makeEvent(22U, 200ULL), SecurityEventPriority::LOW);

    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_NOT_NULL(q.peek());
        TEST_ASSERT_EQUAL_PTR(q.peek(), q.front());
        TEST_ASSERT_EQUAL_UINT32(11U, q.peek()->event_id);
        TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(q.size()));
    }

    // Failed send does NOT pop; new incoming event can still arrive.
    TestSecurityEventPod staged{};
    SecurityEventPriority staged_prio = SecurityEventPriority::LOW;
    TEST_ASSERT_TRUE(q.peek(&staged, &staged_prio));
    TEST_ASSERT_EQUAL_UINT32(11U, staged.event_id);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityEventPriority::HIGH),
                            static_cast<uint8_t>(staged_prio));
    bool ack_ok = false;
    if (ack_ok) {
        TEST_ASSERT_TRUE(q.pop());
    }
    q.push(makeEvent(33U, 300ULL), SecurityEventPriority::HIGH);
    TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(q.size()));
    TEST_ASSERT_EQUAL_UINT32(11U, q.peek()->event_id);

    // Successful ACK pops exactly one event.
    ack_ok = true;
    if (ack_ok) {
        TEST_ASSERT_TRUE(q.pop());
    }
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(q.size()));
    TEST_ASSERT_EQUAL_UINT32(22U, q.peek()->event_id);
}

void test_value_ownership_self_push_and_nan_invalid_preservation(void) {
    BoundedSecurityEventQueue<TestSecurityEventPod, 3U> q;
    {
        TestSecurityEventPod local_evt = makeEvent(77U, 7700ULL, false, "door_fault_nan");
        q.push(local_evt, SecurityEventPriority::HIGH);
        local_evt.event_id = 0U;
        local_evt.valid = true;
        local_evt.aux_value = 0.0f;
        std::memset(local_evt.note, 'Z', sizeof(local_evt.note) - 1U);
        local_evt.note[sizeof(local_evt.note) - 1U] = '\0';
    }

    TEST_ASSERT_EQUAL_UINT32(77U, q.peek()->event_id);
    TEST_ASSERT_FALSE(q.peek()->valid);
    TEST_ASSERT_TRUE(std::isnan(q.peek()->aux_value));
    TEST_ASSERT_EQUAL_STRING("door_fault_nan", q.peek()->note);

    // Self-push when full (*q.peek() aliases head).
    q.push(makeEvent(88U, 8800ULL, true, "second"), SecurityEventPriority::LOW);
    q.push(makeEvent(99U, 9900ULL, true, "third"), SecurityEventPriority::HIGH);
    TEST_ASSERT_TRUE(q.full());
    const auto self_res = q.push(*q.peek(), SecurityEventPriority::HIGH);
    TEST_ASSERT_TRUE(self_res.evicted_old);  // Evicts L(88), retains [H(77), H(99), H(77)].

    TestSecurityEventPod out{};
    TEST_ASSERT_TRUE(q.pop(&out));
    TEST_ASSERT_EQUAL_UINT32(77U, out.event_id);
    TEST_ASSERT_FALSE(out.valid);
    TEST_ASSERT_TRUE(std::isnan(out.aux_value));
    TEST_ASSERT_TRUE(q.pop(&out));
    TEST_ASSERT_EQUAL_UINT32(99U, out.event_id);
    TEST_ASSERT_TRUE(q.pop(&out));
    TEST_ASSERT_EQUAL_UINT32(77U, out.event_id);
    TEST_ASSERT_EQUAL_STRING("door_fault_nan", out.note);

    // Queue copy and copy-assignment remain independent.
    q.push(makeEvent(123U, 1230ULL), SecurityEventPriority::LOW);
    BoundedSecurityEventQueue<TestSecurityEventPod, 3U> q_copy(q);
    TEST_ASSERT_TRUE(q_copy.pop());
    TEST_ASSERT_TRUE(q_copy.empty());
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(q.size()));
    q_copy = q;
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(q_copy.size()));
    TEST_ASSERT_EQUAL_UINT32(123U, q_copy.peek()->event_id);
}

void test_payload_traits_accept_trivial_pod_and_security_state(void) {
    TEST_ASSERT_TRUE(is_valid_security_queue_payload<PureTrivialSecurityPod>::value);
    TEST_ASSERT_TRUE(is_valid_security_queue_payload<SecurityState>::value);
    TEST_ASSERT_FALSE(is_valid_security_queue_payload<std::string>::value);
    TEST_ASSERT_FALSE(is_valid_security_queue_payload<std::vector<uint8_t>>::value);
    TEST_ASSERT_FALSE(is_valid_security_queue_payload<CustomHeapCopyEvent>::value);
    TEST_ASSERT_FALSE(is_valid_security_queue_payload<CustomCopyAssignOnlyEvent>::value);
    TEST_ASSERT_FALSE(is_valid_security_queue_payload<StructWithStdStringEvent>::value);
    TEST_ASSERT_FALSE(is_valid_security_queue_payload<int*>::value);

    BoundedSecurityEventQueue<PureTrivialSecurityPod, 4U> pod_q;
    TEST_ASSERT_TRUE(
        (BoundedSecurityEventQueue<PureTrivialSecurityPod, 4U>::kIsTriviallyDefaultConstructible));
    pod_q.push(PureTrivialSecurityPod{100ULL, 1U, 3.5f, true}, SecurityEventPriority::LOW);
    pod_q.push(PureTrivialSecurityPod{200ULL, 2U, std::numeric_limits<float>::quiet_NaN(), false},
               SecurityEventPriority::HIGH);

    PureTrivialSecurityPod out{};
    TEST_ASSERT_TRUE(pod_q.pop(&out));
    TEST_ASSERT_EQUAL_UINT32(1U, out.event_id);
    TEST_ASSERT_TRUE(out.valid);
    TEST_ASSERT_TRUE(pod_q.pop(&out));
    TEST_ASSERT_EQUAL_UINT32(2U, out.event_id);
    TEST_ASSERT_FALSE(out.valid);
    TEST_ASSERT_TRUE(std::isnan(out.metric));

    BoundedSecurityEventQueue<SecurityState, 4U> state_q;
    SecurityState s{};
    s.door_open = true;
    s.alarm_on = true;
    state_q.push(s, SecurityEventPriority::HIGH);
    SecurityState s_out{};
    TEST_ASSERT_TRUE(state_q.pop(&s_out));
    TEST_ASSERT_TRUE(s_out.door_open);
    TEST_ASSERT_TRUE(s_out.alarm_on);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_capacity_one_priority_evict_and_reject);
    RUN_TEST(test_capacity_20_default_fifo_and_all_same_priority_overflow);
    RUN_TEST(test_mixed_priority_middle_eviction_and_fifo_order_with_wraparound);
    RUN_TEST(test_wrap_around_and_sequence_ordering_across_many_evictions);
    RUN_TEST(test_counter_saturation_for_evicted_rejected_and_total_dropped);
    RUN_TEST(test_clear_preserves_counters_and_reset_counters_clears_stats);
    RUN_TEST(test_peek_non_consuming_failed_send_retains_and_ack_pops_one);
    RUN_TEST(test_value_ownership_self_push_and_nan_invalid_preservation);
    RUN_TEST(test_payload_traits_accept_trivial_pod_and_security_state);
    return UNITY_END();
}
