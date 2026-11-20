#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace node_sec {

// Caller-supplied retention priority for bounded buffer eviction.
// This is strictly an internal queue retention policy; it does NOT infer priority from GPIO,
// PIR, or HOME/AWAY state and does NOT alter FIFO delivery order of retained events.
enum class SecurityEventPriority : uint8_t {
    LOW = 0,
    HIGH = 1
};

enum class SecurityQueuePushStatus : uint8_t {
    STORED = 0,
    EVICTED_OLD = 1,
    REJECTED_INCOMING = 2
};

const char* securityEventPriorityToString(SecurityEventPriority priority);
const char* securityQueuePushStatusToString(SecurityQueuePushStatus status);

template <typename CounterT = uint64_t>
struct SecurityQueuePushResult {
    SecurityQueuePushStatus status = SecurityQueuePushStatus::STORED;
    bool stored = false;
    bool evicted_old = false;
    bool rejected = false;
    std::size_t size_after = 0U;
    CounterT evicted_count_after = 0U;
    CounterT rejected_count_after = 0U;
};

template <typename T>
struct is_valid_security_queue_payload
    : std::integral_constant<
          bool,
          !std::is_pointer<T>::value &&
              !std::is_reference<T>::value &&
              !std::is_const<T>::value &&
              !std::is_volatile<T>::value &&
              !std::is_array<T>::value &&
              std::is_default_constructible<T>::value &&
              std::is_trivially_copyable<T>::value &&
              std::is_trivially_copy_constructible<T>::value &&
              std::is_trivially_copy_assignable<T>::value &&
              std::is_trivially_destructible<T>::value> {};

template <typename T>
inline constexpr bool is_valid_security_queue_payload_v =
    is_valid_security_queue_payload<T>::value;

// Pure C++17 fixed-capacity FIFO queue with caller-supplied priority-aware eviction when full.
//
// Priority & Eviction Contract:
// - Incoming `priority` is provided explicitly by the caller (LOW or HIGH).
// - Retained events are ALWAYS popped in FIFO arrival order (no priority reordering).
// - When full:
//   - If incoming priority is strictly lower than ALL stored events, incoming is REJECTED_INCOMING,
//     stored events remain untouched, and `rejected_count()` saturating-increments.
//   - Otherwise, the OLDEST stored event at the LOWEST stored priority level is evicted
//     (shifting subsequent retained entries cleanly to preserve FIFO relative order), incoming
//     is appended at the tail, and `evicted_count()` saturating-increments.
//
// Supported Payload Contract (`T`):
// - `T` must be a non-pointer, non-reference, non-array, non-cv-qualified, default-constructible
//   value-only snapshot that is trivially copyable, trivially copy-constructible, trivially
//   copy-assignable, and trivially destructible (`std::string`, `std::vector`, Arduino `String`,
//   and types with user-defined/allocating copy or destructor are rejected at compile time).
// - Scope of zero-heap guarantee: `push`, `peek`, `front`, `pop`, and `clear` perform strictly
//   trivial copies/assignments (and `default_payload_` resets) with zero heap allocation and never
//   invoke `T::T()`. During `BoundedSecurityEventQueue` construction, `slots_{}` and
//   `default_payload_{}` invoke `T()`. If `T` is not `std::is_trivially_default_constructible<T>`,
//   callers must ensure `T::T()` does not allocate heap.
// - Raw pointer member limitation: `std::is_trivially_copyable` does NOT forbid raw pointer fields
//   inside a struct (`struct { char* p; }` would shallow-copy pointers). Payloads MUST be
//   value-only snapshots without owning pointers or dynamically allocated resources.
//
// Reference / Pointer Validity Contract:
// - `peek()` and `front()` return a non-owning pointer to the internal head payload slot.
// - That pointer remains valid ONLY until the next mutating call (`push`, `pop`, `clear`) or
//   destruction/assignment of the queue. Call `peek(&copy)` for a value copy across mutations.
//
// Clear vs Statistics Contract:
// - `clear()` empties all buffered events while PRESERVING `evicted_count()` and
//   `rejected_count()`. Call `reset_counters()` explicitly to zero statistics.
template <typename T, std::size_t Capacity = 20U, typename CounterT = uint64_t>
class BoundedSecurityEventQueue {
    static_assert(Capacity > 0U, "BoundedSecurityEventQueue requires compile-time Capacity > 0.");
    static_assert(Capacity < std::numeric_limits<std::size_t>::max(),
                  "BoundedSecurityEventQueue Capacity exceeds safe index arithmetic bound.");
    // Enforce standard type traits directly alongside `is_valid_security_queue_payload<T>` so
    // trait specialization cannot silently bypass zero-heap invariants.
    static_assert(!std::is_pointer<T>::value,
                  "BoundedSecurityEventQueue requires value ownership; raw pointers are forbidden.");
    static_assert(!std::is_reference<T>::value,
                  "BoundedSecurityEventQueue element type cannot be a reference.");
    static_assert(!std::is_const<T>::value && !std::is_volatile<T>::value,
                  "BoundedSecurityEventQueue element type must not be const or volatile qualified.");
    static_assert(!std::is_array<T>::value,
                  "BoundedSecurityEventQueue element type must not be a raw C array.");
    static_assert(std::is_default_constructible<T>::value,
                  "BoundedSecurityEventQueue element type must be default-constructible.");
    static_assert(std::is_trivially_copyable<T>::value,
                  "BoundedSecurityEventQueue requires trivially copyable value-only payload.");
    static_assert(std::is_trivially_copy_constructible<T>::value &&
                      std::is_trivially_copy_assignable<T>::value,
                  "BoundedSecurityEventQueue rejects non-trivial or allocating copy operations.");
    static_assert(std::is_trivially_destructible<T>::value,
                  "BoundedSecurityEventQueue requires trivially destructible value-only payload.");
    static_assert(is_valid_security_queue_payload<T>::value,
                  "BoundedSecurityEventQueue payload failed is_valid_security_queue_payload.");
    static_assert(std::is_unsigned<CounterT>::value && !std::is_same<CounterT, bool>::value,
                  "BoundedSecurityEventQueue CounterT must be an unsigned integer type.");

public:
    using value_type = T;
    using counter_type = CounterT;
    using PushResult = SecurityQueuePushResult<CounterT>;

    static constexpr std::size_t kCapacity = Capacity;
    static constexpr bool kIsTriviallyDefaultConstructible =
        std::is_trivially_default_constructible<T>::value;

    BoundedSecurityEventQueue() = default;

    constexpr std::size_t capacity() const noexcept { return Capacity; }
    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0U; }
    bool full() const noexcept { return count_ == Capacity; }
    CounterT evicted_count() const noexcept { return evicted_count_; }
    CounterT rejected_count() const noexcept { return rejected_count_; }

    // Saturated sum of `evicted_count() + rejected_count()` without unsigned wrap-around.
    CounterT total_dropped_count() const noexcept {
        constexpr CounterT kMax = std::numeric_limits<CounterT>::max();
        return (evicted_count_ > static_cast<CounterT>(kMax - rejected_count_))
                   ? kMax
                   : static_cast<CounterT>(evicted_count_ + rejected_count_);
    }

    // Stores `payload` with explicit caller `priority`.
    // When full, rejects incoming if `priority < lowest_stored_priority`; otherwise evicts the
    // oldest stored event at the lowest stored priority and appends incoming at the FIFO tail.
    PushResult push(const T& payload,
                    SecurityEventPriority priority = SecurityEventPriority::LOW) {
        // Copy first so self-referential push e.g. queue.push(*queue.peek(), prio) stays valid.
        const T value_copy(payload);
        const SecurityEventPriority norm_prio = normalizePriority(priority);
        PushResult result{};

        if (count_ < Capacity) {
            slots_[tail_].payload = value_copy;
            slots_[tail_].priority = norm_prio;
            tail_ = nextIndex(tail_);
            ++count_;

            result.status = SecurityQueuePushStatus::STORED;
            result.stored = true;
            result.evicted_old = false;
            result.rejected = false;
            result.size_after = count_;
            result.evicted_count_after = evicted_count_;
            result.rejected_count_after = rejected_count_;
            return result;
        }

        // Queue is full (count_ == Capacity): find oldest entry with lowest stored priority.
        std::size_t victim_logical = 0U;
        SecurityEventPriority lowest_prio = slots_[head_].priority;
        for (std::size_t logical = 1U; logical < Capacity; ++logical) {
            const SecurityEventPriority p = slots_[physicalIndex(logical)].priority;
            if (priorityRank(p) < priorityRank(lowest_prio)) {
                lowest_prio = p;
                victim_logical = logical;
                if (lowest_prio == SecurityEventPriority::LOW) {
                    break;  // Earliest LOW is strictly the oldest lowest-priority victim.
                }
            }
        }

        if (priorityRank(norm_prio) < priorityRank(lowest_prio)) {
            incrementSaturated(rejected_count_);
            result.status = SecurityQueuePushStatus::REJECTED_INCOMING;
            result.stored = false;
            result.evicted_old = false;
            result.rejected = true;
            result.size_after = count_;
            result.evicted_count_after = evicted_count_;
            result.rejected_count_after = rejected_count_;
            return result;
        }

        if (victim_logical == 0U) {
            // Fast path: head is the victim (works for Capacity == 1 and uniform priorities).
            slots_[head_].payload = value_copy;
            slots_[head_].priority = norm_prio;
            head_ = nextIndex(head_);
            tail_ = nextIndex(tail_);
        } else {
            // Middle/tail eviction: bounded left shift preserves FIFO order of retained items.
            for (std::size_t k = victim_logical; (k + 1U) < Capacity; ++k) {
                slots_[physicalIndex(k)] = slots_[physicalIndex(k + 1U)];
            }
            const std::size_t last_phys = physicalIndex(Capacity - 1U);
            slots_[last_phys].payload = value_copy;
            slots_[last_phys].priority = norm_prio;
        }

        incrementSaturated(evicted_count_);
        result.status = SecurityQueuePushStatus::EVICTED_OLD;
        result.stored = true;
        result.evicted_old = true;
        result.rejected = false;
        result.size_after = count_;
        result.evicted_count_after = evicted_count_;
        result.rejected_count_after = rejected_count_;
        return result;
    }

    // Non-consuming inspection of oldest retained payload. Returns nullptr if empty.
    // Pointer becomes invalid upon any mutating call (`push`, `pop`, `clear`) or destruction.
    const T* peek() const noexcept {
        if (count_ == 0U) {
            return nullptr;
        }
        return &slots_[head_].payload;
    }

    // Alias for `peek()`; never consumes the head element.
    const T* front() const noexcept { return peek(); }

    // Non-consuming value-copy peek. Returns false if empty or `out_payload == nullptr`.
    bool peek(T* out_payload, SecurityEventPriority* out_priority = nullptr) const {
        if (out_payload == nullptr || count_ == 0U) {
            return false;
        }
        *out_payload = slots_[head_].payload;
        if (out_priority != nullptr) {
            *out_priority = slots_[head_].priority;
        }
        return true;
    }

    // Removes the oldest retained event after caller confirms processing/ACK.
    bool pop() {
        if (count_ == 0U) {
            return false;
        }
        slots_[head_].payload = default_payload_;
        slots_[head_].priority = SecurityEventPriority::LOW;
        head_ = nextIndex(head_);
        --count_;
        return true;
    }

    // Copies and removes the oldest retained event. Returns false if empty or `out_payload == nullptr`.
    bool pop(T* out_payload, SecurityEventPriority* out_priority = nullptr) {
        if (out_payload == nullptr || count_ == 0U) {
            return false;
        }
        *out_payload = slots_[head_].payload;
        if (out_priority != nullptr) {
            *out_priority = slots_[head_].priority;
        }
        slots_[head_].payload = default_payload_;
        slots_[head_].priority = SecurityEventPriority::LOW;
        head_ = nextIndex(head_);
        --count_;
        return true;
    }

    // Empties all buffered events while PRESERVING `evicted_count()` and `rejected_count()`.
    void clear() {
        for (std::size_t i = 0U; i < Capacity; ++i) {
            slots_[i].payload = default_payload_;
            slots_[i].priority = SecurityEventPriority::LOW;
        }
        head_ = 0U;
        tail_ = 0U;
        count_ = 0U;
    }

    // Explicitly resets cumulative eviction and rejection counters without altering stored events.
    void reset_counters() noexcept {
        evicted_count_ = 0U;
        rejected_count_ = 0U;
    }

private:
    struct Slot {
        T payload{};
        SecurityEventPriority priority = SecurityEventPriority::LOW;
    };

    T default_payload_{};
    std::array<Slot, Capacity> slots_{};
    std::size_t head_ = 0U;
    std::size_t tail_ = 0U;
    std::size_t count_ = 0U;
    CounterT evicted_count_ = 0U;
    CounterT rejected_count_ = 0U;

    static constexpr SecurityEventPriority normalizePriority(
        SecurityEventPriority priority) noexcept {
        return (priority == SecurityEventPriority::HIGH) ? SecurityEventPriority::HIGH
                                                         : SecurityEventPriority::LOW;
    }

    static constexpr uint8_t priorityRank(SecurityEventPriority priority) noexcept {
        return (priority == SecurityEventPriority::HIGH) ? 1U : 0U;
    }

    static constexpr std::size_t nextIndex(std::size_t idx) noexcept {
        return (idx + 1U == Capacity) ? 0U : (idx + 1U);
    }

    std::size_t physicalIndex(std::size_t logical_offset) const noexcept {
        const std::size_t rem = Capacity - head_;
        return (logical_offset < rem) ? (head_ + logical_offset) : (logical_offset - rem);
    }

    static void incrementSaturated(CounterT& counter) noexcept {
        if (counter < std::numeric_limits<CounterT>::max()) {
            ++counter;
        }
    }
};

}  // namespace node_sec
