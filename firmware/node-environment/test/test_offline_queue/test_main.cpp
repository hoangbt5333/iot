#include <unity.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "node_automation.h"
#include "offline_fifo_queue.h"

using node_env::BoundedFifoQueue;
using node_env::QueuePushStatus;
using node_env::SensorReading;
using node_env::is_valid_queue_payload;
using node_env::is_valid_queue_payload_v;
using node_env::queuePushStatusToString;

namespace {

// Pure C-style POD (trivially default-constructible, trivially copyable, trivially destructible).
struct PureTrivialPod {
    uint64_t uptime_ms;
    int32_t seq;
    float value;
    bool valid;
};

// Custom type whose copy constructor/assignment dynamically allocates memory (MUST be rejected).
struct CustomHeapCopyType {
    char* buffer = nullptr;
    size_t len = 0U;

    CustomHeapCopyType() = default;
    CustomHeapCopyType(const CustomHeapCopyType& other) : len(other.len) {
        if (other.buffer != nullptr) {
            buffer = new char[len + 1U];
            std::memcpy(buffer, other.buffer, len + 1U);
        }
    }
    CustomHeapCopyType& operator=(const CustomHeapCopyType& other) {
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
    ~CustomHeapCopyType() { delete[] buffer; }
};

// Custom type with non-trivial copy assignment even with trivial destructor (MUST be rejected).
struct CustomCopyAssignOnlyType {
    int32_t code = 0;
    CustomCopyAssignOnlyType() = default;
    CustomCopyAssignOnlyType(const CustomCopyAssignOnlyType&) = default;
    CustomCopyAssignOnlyType& operator=(const CustomCopyAssignOnlyType& other) {
        code = other.code;
        return *this;
    }
    ~CustomCopyAssignOnlyType() = default;
};

struct StructWithStdString {
    uint32_t seq = 0U;
    std::string text;
};

// Test-only POD snapshot model (NOT a frozen production telemetry/ThingsBoard contract).
struct TestSnapshotPod {
    uint64_t uptime_ms = 0ULL;
    bool valid = false;
    bool has_temperature = false;
    float temperature_c = 0.0f;
    bool has_humidity = false;
    float humidity_pct = 0.0f;
    bool has_light_lux = false;
    float light_lux = 0.0f;
    bool has_light_raw = false;
    int32_t light_raw = 0;
    uint32_t fault_mask = 0U;
    char payload[48]{};
};

// Compile-time payload contract checks: valid value-only snapshots accepted; heap/pointer types rejected.
static_assert(is_valid_queue_payload<PureTrivialPod>::value, "PureTrivialPod must be valid.");
static_assert(is_valid_queue_payload<TestSnapshotPod>::value, "TestSnapshotPod must be valid.");
static_assert(is_valid_queue_payload<SensorReading>::value, "SensorReading must be valid.");
static_assert(is_valid_queue_payload<int32_t>::value, "Scalar integer must be valid.");
static_assert(is_valid_queue_payload_v<uint64_t>, "Variable template trait must accept uint64_t.");

static_assert(!is_valid_queue_payload<std::string>::value, "std::string must be rejected.");
static_assert(!is_valid_queue_payload<std::vector<uint8_t>>::value,
              "std::vector must be rejected.");
static_assert(!is_valid_queue_payload<CustomHeapCopyType>::value,
              "Custom heap-copy type must be rejected.");
static_assert(!is_valid_queue_payload<CustomCopyAssignOnlyType>::value,
              "Custom non-trivial copy-assign type must be rejected.");
static_assert(!is_valid_queue_payload<StructWithStdString>::value,
              "Struct containing std::string must be rejected.");
static_assert(!is_valid_queue_payload<int*>::value, "Raw pointer int* must be rejected.");
static_assert(!is_valid_queue_payload<const char*>::value, "Raw pointer const char* must be rejected.");
static_assert(!is_valid_queue_payload<TestSnapshotPod&>::value, "Reference type must be rejected.");
static_assert(!is_valid_queue_payload<const TestSnapshotPod>::value, "const type must be rejected.");
static_assert(!is_valid_queue_payload<char[16]>::value, "Raw array type must be rejected.");
static_assert(!is_valid_queue_payload<void>::value, "void must be rejected.");

TestSnapshotPod makeTestPod(uint64_t uptime_ms,
                            bool valid,
                            const char* tag,
                            float temp_c = 28.5f,
                            float hum_pct = 61.0f) {
    TestSnapshotPod pod{};
    pod.uptime_ms = uptime_ms;
    pod.valid = valid;
    pod.has_temperature = valid;
    pod.temperature_c = temp_c;
    pod.has_humidity = valid;
    pod.humidity_pct = hum_pct;
    pod.has_light_lux = valid;
    pod.light_lux = 120.0f;
    pod.has_light_raw = false;
    pod.light_raw = 0;
    pod.fault_mask = valid ? 0U : 1U;
    if (tag != nullptr) {
        const size_t len = std::strlen(tag);
        const size_t copy_len = (len < sizeof(pod.payload) - 1U) ? len : (sizeof(pod.payload) - 1U);
        if (copy_len > 0U) {
            std::memcpy(pod.payload, tag, copy_len);
        }
        pod.payload[copy_len] = '\0';
    }
    return pod;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_capacity_one_lifecycle_and_overflow(void) {
    BoundedFifoQueue<TestSnapshotPod, 1U> queue;

    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(queue.capacity()));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_FALSE(queue.full());
    TEST_ASSERT_EQUAL_UINT64(0ULL, queue.dropped_count());
    TEST_ASSERT_NULL(queue.peek());
    TEST_ASSERT_NULL(queue.front());
    TEST_ASSERT_FALSE(queue.pop());

    TestSnapshotPod copy_out = makeTestPod(999ULL, true, "sentinel");
    TEST_ASSERT_FALSE(queue.peek(&copy_out));
    TEST_ASSERT_EQUAL_UINT64(999ULL, copy_out.uptime_ms);
    TEST_ASSERT_FALSE(queue.pop(&copy_out));
    TEST_ASSERT_EQUAL_UINT64(999ULL, copy_out.uptime_ms);

    const auto p1 = queue.push(makeTestPod(1000ULL, true, "first"));
    TEST_ASSERT_FALSE(p1.dropped_oldest);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(QueuePushStatus::ENQUEUED),
                            static_cast<uint8_t>(p1.status));
    TEST_ASSERT_EQUAL_STRING("enqueued", queuePushStatusToString(p1.status));
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_FALSE(queue.empty());
    TEST_ASSERT_TRUE(queue.full());
    TEST_ASSERT_EQUAL_UINT64(0ULL, queue.dropped_count());
    TEST_ASSERT_NOT_NULL(queue.peek());
    TEST_ASSERT_EQUAL_PTR(queue.peek(), queue.front());
    TEST_ASSERT_EQUAL_UINT64(1000ULL, queue.peek()->uptime_ms);

    const auto p2 = queue.push(makeTestPod(2000ULL, true, "second"));
    TEST_ASSERT_TRUE(p2.dropped_oldest);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(QueuePushStatus::ENQUEUED_DROPPED_OLDEST),
                            static_cast<uint8_t>(p2.status));
    TEST_ASSERT_EQUAL_STRING("enqueued_dropped_oldest", queuePushStatusToString(p2.status));
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_TRUE(queue.full());
    TEST_ASSERT_EQUAL_UINT64(1ULL, queue.dropped_count());
    TEST_ASSERT_EQUAL_UINT64(2000ULL, queue.front()->uptime_ms);
    TEST_ASSERT_EQUAL_STRING("second", queue.peek()->payload);

    const auto p3 = queue.push(makeTestPod(3000ULL, false, "third"));
    TEST_ASSERT_TRUE(p3.dropped_oldest);
    TEST_ASSERT_EQUAL_UINT64(2ULL, queue.dropped_count());
    TEST_ASSERT_EQUAL_UINT64(3000ULL, queue.peek()->uptime_ms);
    TEST_ASSERT_FALSE(queue.peek()->valid);

    TestSnapshotPod popped{};
    TEST_ASSERT_TRUE(queue.pop(&popped));
    TEST_ASSERT_EQUAL_UINT64(3000ULL, popped.uptime_ms);
    TEST_ASSERT_EQUAL_STRING("third", popped.payload);
    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_FALSE(queue.full());
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_EQUAL_UINT64(2ULL, queue.dropped_count());
    TEST_ASSERT_NULL(queue.peek());
    TEST_ASSERT_FALSE(queue.pop());
}

void test_capacity_24_fifo_and_drop_oldest_overflow(void) {
    BoundedFifoQueue<TestSnapshotPod, 24U> queue;
    TEST_ASSERT_EQUAL_UINT32(24U, static_cast<uint32_t>(queue.capacity()));

    for (uint32_t i = 0U; i < 24U; ++i) {
        char buf[24]{};
        std::snprintf(buf, sizeof(buf), "item-%02lu", static_cast<unsigned long>(i));
        const auto res = queue.push(makeTestPod(1000ULL + i, true, buf));
        TEST_ASSERT_FALSE(res.dropped_oldest);
        TEST_ASSERT_EQUAL_UINT32(i + 1U, static_cast<uint32_t>(res.size_after));
        TEST_ASSERT_EQUAL_UINT64(0ULL, res.dropped_count_after);
    }

    TEST_ASSERT_TRUE(queue.full());
    TEST_ASSERT_EQUAL_UINT32(24U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_EQUAL_UINT64(0ULL, queue.dropped_count());
    TEST_ASSERT_EQUAL_UINT64(1000ULL, queue.peek()->uptime_ms);

    // Push 10 additional samples -> oldest 0..9 dropped, 10..33 remain in strict FIFO order.
    for (uint32_t i = 24U; i < 34U; ++i) {
        char buf[24]{};
        std::snprintf(buf, sizeof(buf), "item-%02lu", static_cast<unsigned long>(i));
        const auto res = queue.push(makeTestPod(1000ULL + i, true, buf));
        TEST_ASSERT_TRUE(res.dropped_oldest);
        TEST_ASSERT_EQUAL_UINT32(24U, static_cast<uint32_t>(res.size_after));
        TEST_ASSERT_EQUAL_UINT64(static_cast<uint64_t>(i - 23U), res.dropped_count_after);
    }

    TEST_ASSERT_EQUAL_UINT64(10ULL, queue.dropped_count());
    TEST_ASSERT_EQUAL_UINT32(24U, static_cast<uint32_t>(queue.size()));

    for (uint32_t expected = 10U; expected < 34U; ++expected) {
        TestSnapshotPod peeked{};
        TEST_ASSERT_TRUE(queue.peek(&peeked));
        TEST_ASSERT_EQUAL_UINT64(1000ULL + expected, peeked.uptime_ms);

        TestSnapshotPod popped{};
        TEST_ASSERT_TRUE(queue.pop(&popped));
        TEST_ASSERT_EQUAL_UINT64(1000ULL + expected, popped.uptime_ms);
    }

    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_EQUAL_UINT64(10ULL, queue.dropped_count());
}

void test_wrap_around_interleaved_push_pop_all_boundaries(void) {
    BoundedFifoQueue<TestSnapshotPod, 24U> queue;
    uint64_t next_push_id = 1ULL;
    uint64_t next_expected_pop_id = 1ULL;

    // Advance head/tail through 20 full-capacity wrap cycles via interleaved batches.
    for (int cycle = 0; cycle < 40; ++cycle) {
        const size_t to_push = (cycle % 2 == 0) ? 17U : 13U;
        for (size_t i = 0U; i < to_push; ++i) {
            const auto res = queue.push(makeTestPod(next_push_id, true, "wrap"));
            if (res.dropped_oldest) {
                ++next_expected_pop_id;
            }
            ++next_push_id;
        }

        const size_t to_pop = (cycle % 3 == 0) ? 11U : 9U;
        for (size_t i = 0U; i < to_pop; ++i) {
            if (queue.empty()) {
                break;
            }
            TestSnapshotPod out{};
            TEST_ASSERT_TRUE(queue.pop(&out));
            TEST_ASSERT_EQUAL_UINT64(next_expected_pop_id, out.uptime_ms);
            ++next_expected_pop_id;
        }
    }

    // Drain remaining items and confirm contiguous FIFO order.
    while (!queue.empty()) {
        TestSnapshotPod out{};
        TEST_ASSERT_TRUE(queue.pop(&out));
        TEST_ASSERT_EQUAL_UINT64(next_expected_pop_id, out.uptime_ms);
        ++next_expected_pop_id;
    }
    TEST_ASSERT_EQUAL_UINT64(next_push_id, next_expected_pop_id);
    TEST_ASSERT_EQUAL_UINT64(next_push_id - 1ULL - 0ULL,
                             (next_expected_pop_id - 1ULL));

    // Specifically test overflowing when head == tail != 0 (exact wrap boundary at index 17).
    BoundedFifoQueue<TestSnapshotPod, 5U> small_q;
    for (uint64_t i = 1ULL; i <= 3ULL; ++i) {
        small_q.push(makeTestPod(i, true, "pre"));
    }
    TEST_ASSERT_TRUE(small_q.pop());
    TEST_ASSERT_TRUE(small_q.pop());  // head = 2, tail = 3, count = 1 (id=3)
    for (uint64_t i = 4ULL; i <= 10ULL; ++i) {
        small_q.push(makeTestPod(i, true, "overflow-non-zero-head"));
    }
    TEST_ASSERT_EQUAL_UINT32(5U, static_cast<uint32_t>(small_q.size()));
    TEST_ASSERT_TRUE(small_q.full());
    TEST_ASSERT_EQUAL_UINT64(3ULL, small_q.dropped_count());
    for (uint64_t expected = 6ULL; expected <= 10ULL; ++expected) {
        TestSnapshotPod out{};
        TEST_ASSERT_TRUE(small_q.pop(&out));
        TEST_ASSERT_EQUAL_UINT64(expected, out.uptime_ms);
    }
    TEST_ASSERT_TRUE(small_q.empty());
}

void test_dropped_count_saturation_policy(void) {
    // Use uint8_t CounterT to exercise real overflow boundary at 255 without test backdoors.
    BoundedFifoQueue<TestSnapshotPod, 2U, uint8_t> small_counter_q;

    small_counter_q.push(makeTestPod(1ULL, true, "a"));
    small_counter_q.push(makeTestPod(2ULL, true, "b"));
    TEST_ASSERT_EQUAL_UINT8(0U, small_counter_q.dropped_count());

    // Push 260 overflowing samples: dropped_count must saturate at 255 (never wrap to 4).
    for (uint32_t i = 3U; i <= 262U; ++i) {
        const auto res = small_counter_q.push(makeTestPod(i, true, "sat"));
        TEST_ASSERT_TRUE(res.dropped_oldest);
        const uint8_t expected = (i - 2U >= 255U) ? 255U : static_cast<uint8_t>(i - 2U);
        TEST_ASSERT_EQUAL_UINT8(expected, small_counter_q.dropped_count());
        TEST_ASSERT_EQUAL_UINT8(expected, res.dropped_count_after);
    }

    TEST_ASSERT_EQUAL_UINT8(std::numeric_limits<uint8_t>::max(), small_counter_q.dropped_count());
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(small_counter_q.size()));

    TestSnapshotPod first{};
    TestSnapshotPod second{};
    TEST_ASSERT_TRUE(small_counter_q.pop(&first));
    TEST_ASSERT_TRUE(small_counter_q.pop(&second));
    TEST_ASSERT_EQUAL_UINT64(261ULL, first.uptime_ms);
    TEST_ASSERT_EQUAL_UINT64(262ULL, second.uptime_ms);
    TEST_ASSERT_EQUAL_UINT8(255U, small_counter_q.dropped_count());

    // Verify default CounterT is uint64_t.
    BoundedFifoQueue<TestSnapshotPod, 24U> default_q;
    const bool counter_is_u64 =
        std::is_same<decltype(default_q.dropped_count()), uint64_t>::value;
    TEST_ASSERT_TRUE(counter_is_u64);
}

void test_clear_preserves_dropped_count_and_resets_fifo(void) {
    BoundedFifoQueue<TestSnapshotPod, 4U> queue;

    for (uint64_t i = 1ULL; i <= 7ULL; ++i) {
        queue.push(makeTestPod(i * 100ULL, true, "pre-clear"));
    }
    TEST_ASSERT_EQUAL_UINT32(4U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_EQUAL_UINT64(3ULL, queue.dropped_count());

    // Clear empties queue but MUST preserve dropped_count statistics.
    queue.clear();
    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_FALSE(queue.full());
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_NULL(queue.peek());
    TEST_ASSERT_NULL(queue.front());
    TEST_ASSERT_FALSE(queue.pop());
    TEST_ASSERT_EQUAL_UINT64(3ULL, queue.dropped_count());

    // Enqueue fresh entries after clear: no stale entries remain and dropped_count stays accurate.
    const auto r1 = queue.push(makeTestPod(800ULL, true, "post-clear-1"));
    const auto r2 = queue.push(makeTestPod(900ULL, true, "post-clear-2"));
    TEST_ASSERT_FALSE(r1.dropped_oldest);
    TEST_ASSERT_FALSE(r2.dropped_oldest);
    TEST_ASSERT_EQUAL_UINT64(3ULL, queue.dropped_count());
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(queue.size()));

    // Explicit stats reset clears dropped_count without touching queued items.
    queue.reset_dropped_count();
    TEST_ASSERT_EQUAL_UINT64(0ULL, queue.dropped_count());
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(queue.size()));

    TestSnapshotPod out{};
    TEST_ASSERT_TRUE(queue.pop(&out));
    TEST_ASSERT_EQUAL_UINT64(800ULL, out.uptime_ms);
    TEST_ASSERT_TRUE(queue.pop(&out));
    TEST_ASSERT_EQUAL_UINT64(900ULL, out.uptime_ms);
}

void test_peek_non_consuming_failed_send_retains_and_ack_pops_one(void) {
    BoundedFifoQueue<TestSnapshotPod, 24U> queue;
    queue.push(makeTestPod(100ULL, true, "msg-1"));
    queue.push(makeTestPod(200ULL, true, "msg-2"));
    queue.push(makeTestPod(300ULL, true, "msg-3"));

    // Repeated peek/front inspections do not consume.
    for (int retry = 0; retry < 5; ++retry) {
        const TestSnapshotPod* head_ptr = queue.peek();
        TEST_ASSERT_NOT_NULL(head_ptr);
        TEST_ASSERT_EQUAL_UINT64(100ULL, head_ptr->uptime_ms);
        TEST_ASSERT_EQUAL_STRING("msg-1", queue.front()->payload);
        TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(queue.size()));
    }

    // Simulated send failure: caller does not pop; head remains intact even as new samples arrive.
    bool simulated_transport_ack = false;
    TestSnapshotPod staged{};
    TEST_ASSERT_TRUE(queue.peek(&staged));
    TEST_ASSERT_EQUAL_UINT64(100ULL, staged.uptime_ms);
    if (simulated_transport_ack) {
        TEST_ASSERT_TRUE(queue.pop());
    }
    queue.push(makeTestPod(400ULL, true, "msg-4"));
    TEST_ASSERT_EQUAL_UINT32(4U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_EQUAL_UINT64(100ULL, queue.peek()->uptime_ms);

    // Simulated successful send + ACK: pop removes exactly one head sample.
    TEST_ASSERT_TRUE(queue.peek(&staged));
    TEST_ASSERT_EQUAL_UINT64(100ULL, staged.uptime_ms);
    simulated_transport_ack = true;
    if (simulated_transport_ack) {
        TEST_ASSERT_TRUE(queue.pop());
    }

    TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_EQUAL_UINT64(200ULL, queue.peek()->uptime_ms);
    TEST_ASSERT_EQUAL_STRING("msg-2", queue.peek()->payload);
}

void test_value_copy_ownership_and_self_push_safety(void) {
    BoundedFifoQueue<TestSnapshotPod, 4U> queue;

    {
        TestSnapshotPod stack_pod = makeTestPod(555ULL, true, "stack-owned-payload");
        queue.push(stack_pod);
        // Mutate and wipe caller stack fields immediately after push.
        stack_pod.uptime_ms = 0ULL;
        stack_pod.valid = false;
        std::memset(stack_pod.payload, 'X', sizeof(stack_pod.payload) - 1U);
        stack_pod.payload[sizeof(stack_pod.payload) - 1U] = '\0';
    }

    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_EQUAL_UINT64(555ULL, queue.peek()->uptime_ms);
    TEST_ASSERT_TRUE(queue.peek()->valid);
    TEST_ASSERT_EQUAL_STRING("stack-owned-payload", queue.peek()->payload);

    // Self-referential push when full (*queue.peek() aliases head slot) must stay valid.
    queue.push(makeTestPod(666ULL, true, "second"));
    queue.push(makeTestPod(777ULL, true, "third"));
    queue.push(makeTestPod(888ULL, true, "fourth"));
    TEST_ASSERT_TRUE(queue.full());

    const auto self_res = queue.push(*queue.peek());
    TEST_ASSERT_TRUE(self_res.dropped_oldest);
    TEST_ASSERT_EQUAL_UINT64(1ULL, queue.dropped_count());

    TestSnapshotPod out{};
    TEST_ASSERT_TRUE(queue.pop(&out));
    TEST_ASSERT_EQUAL_UINT64(666ULL, out.uptime_ms);
    TEST_ASSERT_TRUE(queue.pop(&out));
    TEST_ASSERT_EQUAL_UINT64(777ULL, out.uptime_ms);
    TEST_ASSERT_TRUE(queue.pop(&out));
    TEST_ASSERT_EQUAL_UINT64(888ULL, out.uptime_ms);
    TEST_ASSERT_TRUE(queue.pop(&out));
    TEST_ASSERT_EQUAL_UINT64(555ULL, out.uptime_ms);
    TEST_ASSERT_EQUAL_STRING("stack-owned-payload", out.payload);

    // Copying and copy-assigning the queue creates an independent value copy.
    queue.push(makeTestPod(111ULL, true, "orig"));
    BoundedFifoQueue<TestSnapshotPod, 4U> queue_copy(queue);
    TEST_ASSERT_TRUE(queue_copy.pop());
    TEST_ASSERT_TRUE(queue_copy.empty());
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(queue.size()));
    TEST_ASSERT_EQUAL_UINT64(111ULL, queue.peek()->uptime_ms);

    queue_copy = queue;
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(queue_copy.size()));
    TEST_ASSERT_EQUAL_UINT64(111ULL, queue_copy.peek()->uptime_ms);
}

void test_payload_trait_and_trivial_pod_sensor_reading_acceptance(void) {
    TEST_ASSERT_TRUE(is_valid_queue_payload<PureTrivialPod>::value);
    TEST_ASSERT_TRUE(is_valid_queue_payload<TestSnapshotPod>::value);
    TEST_ASSERT_TRUE(is_valid_queue_payload<SensorReading>::value);
    TEST_ASSERT_TRUE(is_valid_queue_payload<int32_t>::value);

    TEST_ASSERT_FALSE(is_valid_queue_payload<std::string>::value);
    TEST_ASSERT_FALSE(is_valid_queue_payload<std::vector<uint8_t>>::value);
    TEST_ASSERT_FALSE(is_valid_queue_payload<CustomHeapCopyType>::value);
    TEST_ASSERT_FALSE(is_valid_queue_payload<CustomCopyAssignOnlyType>::value);
    TEST_ASSERT_FALSE(is_valid_queue_payload<StructWithStdString>::value);
    TEST_ASSERT_FALSE(is_valid_queue_payload<int*>::value);
    TEST_ASSERT_FALSE(is_valid_queue_payload<const char*>::value);

    BoundedFifoQueue<PureTrivialPod, 4U> pod_q;
    TEST_ASSERT_TRUE((BoundedFifoQueue<PureTrivialPod, 4U>::kIsTriviallyDefaultConstructible));
    pod_q.push(PureTrivialPod{1000ULL, 1, 27.5f, true});
    pod_q.push(PureTrivialPod{2000ULL, 2, std::numeric_limits<float>::quiet_NaN(), false});
    PureTrivialPod p_out{};
    TEST_ASSERT_TRUE(pod_q.pop(&p_out));
    TEST_ASSERT_EQUAL_INT32(1, p_out.seq);
    TEST_ASSERT_TRUE(p_out.valid);
    TEST_ASSERT_TRUE(pod_q.pop(&p_out));
    TEST_ASSERT_EQUAL_INT32(2, p_out.seq);
    TEST_ASSERT_FALSE(p_out.valid);
    TEST_ASSERT_TRUE(std::isnan(p_out.value));

    BoundedFifoQueue<SensorReading, 4U> sensor_q;
    SensorReading invalid_reading{};
    invalid_reading.temperature_c = std::numeric_limits<float>::quiet_NaN();
    invalid_reading.temperature_valid = false;
    invalid_reading.light_lux = 0.0f;
    invalid_reading.light_lux_valid = false;
    sensor_q.push(invalid_reading);

    SensorReading s_out{};
    TEST_ASSERT_TRUE(sensor_q.pop(&s_out));
    TEST_ASSERT_FALSE(s_out.temperature_valid);
    TEST_ASSERT_TRUE(std::isnan(s_out.temperature_c));
    TEST_ASSERT_FALSE(s_out.light_lux_valid);

    BoundedFifoQueue<int32_t, 3U> int_q;
    int_q.push(-42);
    int_q.push(0);
    int_q.push(77);
    int_q.push(99);
    int32_t i_out = 0;
    TEST_ASSERT_TRUE(int_q.pop(&i_out));
    TEST_ASSERT_EQUAL_INT32(0, i_out);
    TEST_ASSERT_TRUE(int_q.pop(&i_out));
    TEST_ASSERT_EQUAL_INT32(77, i_out);
    TEST_ASSERT_TRUE(int_q.pop(&i_out));
    TEST_ASSERT_EQUAL_INT32(99, i_out);
}

void test_preserves_invalid_nan_and_sim_ldr_fields_without_mutation(void) {
    BoundedFifoQueue<TestSnapshotPod, 24U> queue;

    // 1. Sensor fault snapshot: valid=false, missing/NaN temp & hum, valid light.
    TestSnapshotPod fault_pod{};
    fault_pod.uptime_ms = 220400ULL;
    fault_pod.valid = false;
    fault_pod.has_temperature = false;
    fault_pod.temperature_c = std::numeric_limits<float>::quiet_NaN();
    fault_pod.has_humidity = false;
    fault_pod.humidity_pct = std::numeric_limits<float>::quiet_NaN();
    fault_pod.has_light_lux = true;
    fault_pod.light_lux = 131.0f;
    fault_pod.has_light_raw = false;
    fault_pod.fault_mask = 0x01U;
    std::strcpy(fault_pod.payload, "TEMP_HUM_READ_FAILED");

    // 2. SIM LDR snapshot: has_light_lux=false (null), raw ADC preserved as-is.
    TestSnapshotPod sim_ldr_pod{};
    sim_ldr_pod.uptime_ms = 85120ULL;
    sim_ldr_pod.valid = true;
    sim_ldr_pod.has_temperature = true;
    sim_ldr_pod.temperature_c = 26.8f;
    sim_ldr_pod.has_humidity = true;
    sim_ldr_pod.humidity_pct = 58.5f;
    sim_ldr_pod.has_light_lux = false;
    sim_ldr_pod.light_lux = std::numeric_limits<float>::quiet_NaN();
    sim_ldr_pod.has_light_raw = true;
    sim_ldr_pod.light_raw = 1840;
    sim_ldr_pod.fault_mask = 0U;
    std::strcpy(sim_ldr_pod.payload, "LDR_SIM_RAW");

    queue.push(fault_pod);
    queue.push(sim_ldr_pod);

    TestSnapshotPod out_fault{};
    TEST_ASSERT_TRUE(queue.pop(&out_fault));
    TEST_ASSERT_EQUAL_UINT64(220400ULL, out_fault.uptime_ms);
    TEST_ASSERT_FALSE(out_fault.valid);
    TEST_ASSERT_FALSE(out_fault.has_temperature);
    TEST_ASSERT_TRUE(std::isnan(out_fault.temperature_c));
    TEST_ASSERT_FALSE(out_fault.has_humidity);
    TEST_ASSERT_TRUE(std::isnan(out_fault.humidity_pct));
    TEST_ASSERT_TRUE(out_fault.has_light_lux);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 131.0f, out_fault.light_lux);
    TEST_ASSERT_EQUAL_UINT32(0x01U, out_fault.fault_mask);
    TEST_ASSERT_EQUAL_STRING("TEMP_HUM_READ_FAILED", out_fault.payload);

    TestSnapshotPod out_sim{};
    TEST_ASSERT_TRUE(queue.pop(&out_sim));
    TEST_ASSERT_EQUAL_UINT64(85120ULL, out_sim.uptime_ms);
    TEST_ASSERT_TRUE(out_sim.valid);
    TEST_ASSERT_FALSE(out_sim.has_light_lux);
    TEST_ASSERT_TRUE(std::isnan(out_sim.light_lux));
    TEST_ASSERT_TRUE(out_sim.has_light_raw);
    TEST_ASSERT_EQUAL_INT32(1840, out_sim.light_raw);
    TEST_ASSERT_EQUAL_STRING("LDR_SIM_RAW", out_sim.payload);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_capacity_one_lifecycle_and_overflow);
    RUN_TEST(test_capacity_24_fifo_and_drop_oldest_overflow);
    RUN_TEST(test_wrap_around_interleaved_push_pop_all_boundaries);
    RUN_TEST(test_dropped_count_saturation_policy);
    RUN_TEST(test_clear_preserves_dropped_count_and_resets_fifo);
    RUN_TEST(test_peek_non_consuming_failed_send_retains_and_ack_pops_one);
    RUN_TEST(test_value_copy_ownership_and_self_push_safety);
    RUN_TEST(test_preserves_invalid_nan_and_sim_ldr_fields_without_mutation);
    RUN_TEST(test_payload_trait_and_trivial_pod_sensor_reading_acceptance);
    return UNITY_END();
}
