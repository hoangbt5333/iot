#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace node_env {

enum class QueuePushStatus : uint8_t {
    ENQUEUED = 0,
    ENQUEUED_DROPPED_OLDEST = 1
};

const char* queuePushStatusToString(QueuePushStatus status);

template <typename CounterT = uint64_t>
struct QueuePushResult {
    QueuePushStatus status = QueuePushStatus::ENQUEUED;
    bool dropped_oldest = false;
    std::size_t size_after = 0U;
    CounterT dropped_count_after = 0U;
};

template <typename T>
struct is_valid_queue_payload
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
inline constexpr bool is_valid_queue_payload_v = is_valid_queue_payload<T>::value;

// Pure C++17 fixed-capacity FIFO ring queue with value ownership and zero heap allocation
// during queue operations (`push`, `pop`, `peek`, `front`, `clear`).
//
// Supported Payload Contract (`T`):
// - `T` must be a non-pointer, non-reference, non-array, non-cv-qualified, default-constructible
//   value-only snapshot that is trivially copyable, trivially copy-constructible, trivially
//   copy-assignable, and trivially destructible (`std::string`, `std::vector`, Arduino `String`,
//   and types with user-defined/allocating copy or destructor are rejected at compile time).
// - Scope of zero-heap guarantee: `push`, `pop`, `peek`, `front`, and `clear` perform strictly
//   trivial copies/assignments from `default_slot_` and never allocate heap or invoke `T::T()`.
//   During `BoundedFifoQueue` object construction, `slots_{}` and `default_slot_{}` invoke `T()`.
//   If `T` is not `std::is_trivially_default_constructible<T>` (e.g. a C++17 struct with inline
//   scalar member defaults), callers must ensure `T::T()` does not allocate heap.
// - Raw pointer member limitation: `std::is_trivially_copyable` does NOT recursively forbid raw
//   pointer members inside a struct (a POD containing `T*` would shallow-copy the pointer).
//   Callers must ensure payload structs are value-only snapshots with no owning pointers or
//   heap-backed handles.
//
// Reference / Pointer Validity Contract:
// - `peek()` and `front()` return a non-owning pointer to the internal slot at the queue head.
// - That pointer remains valid ONLY until the next mutating call (`push`, `pop`, `clear`)
//   or destruction/assignment of the queue.
// - Specifically, `pop()` and `clear()` overwrite the slot with `default_slot_`, and `push()`
//   while `full()` overwrites the oldest head slot. Callers needing stability across queue
//   mutations should use `peek(T* out_item)` to obtain an independent value copy.
//
// Clear vs Statistics Contract:
// - `clear()` empties all buffered snapshots while PRESERVING `dropped_count()` so cumulative
//   session drop statistics are never silently erased.
// - Call `reset_dropped_count()` explicitly when starting a new diagnostic counter window.
template <typename T, std::size_t Capacity, typename CounterT = uint64_t>
class BoundedFifoQueue {
    static_assert(Capacity > 0U, "BoundedFifoQueue requires compile-time Capacity > 0.");
    static_assert(Capacity < std::numeric_limits<std::size_t>::max(),
                  "BoundedFifoQueue Capacity exceeds safe index arithmetic bound.");
    // Enforce all standard traits directly as well as `is_valid_queue_payload<T>` to prevent any
    // accidental trait specialization from bypassing zero-heap copy/destruct invariants.
    static_assert(!std::is_pointer<T>::value,
                  "BoundedFifoQueue requires value ownership; raw pointer elements are forbidden.");
    static_assert(!std::is_reference<T>::value,
                  "BoundedFifoQueue element type cannot be a reference.");
    static_assert(!std::is_const<T>::value && !std::is_volatile<T>::value,
                  "BoundedFifoQueue element type must not be const or volatile qualified.");
    static_assert(!std::is_array<T>::value,
                  "BoundedFifoQueue element type must not be a raw C array.");
    static_assert(std::is_default_constructible<T>::value,
                  "BoundedFifoQueue element type must be default-constructible.");
    static_assert(std::is_trivially_copyable<T>::value,
                  "BoundedFifoQueue requires trivially copyable value-only snapshot payload.");
    static_assert(std::is_trivially_copy_constructible<T>::value &&
                      std::is_trivially_copy_assignable<T>::value,
                  "BoundedFifoQueue rejects types with non-trivial or allocating copy operations.");
    static_assert(std::is_trivially_destructible<T>::value,
                  "BoundedFifoQueue requires trivially destructible value-only snapshot payload.");
    static_assert(is_valid_queue_payload<T>::value,
                  "BoundedFifoQueue payload type failed is_valid_queue_payload contract.");
    static_assert(std::is_unsigned<CounterT>::value && !std::is_same<CounterT, bool>::value,
                  "BoundedFifoQueue CounterT must be an unsigned integer type.");

public:
    using value_type = T;
    using counter_type = CounterT;
    using PushResult = QueuePushResult<CounterT>;

    static constexpr std::size_t kCapacity = Capacity;
    static constexpr bool kIsTriviallyDefaultConstructible =
        std::is_trivially_default_constructible<T>::value;

    BoundedFifoQueue() = default;

    constexpr std::size_t capacity() const noexcept { return Capacity; }
    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0U; }
    bool full() const noexcept { return count_ == Capacity; }
    CounterT dropped_count() const noexcept { return dropped_count_; }

    // Enqueues a value copy of `item`. When full, drops the oldest item (advancing head),
    // saturating-increments `dropped_count()`, and preserves FIFO ordering of remaining items.
    PushResult push(const T& item) {
        // Copy first so aliasing e.g. queue.push(*queue.peek()) remains well-defined.
        const T value_copy(item);
        PushResult result;

        if (count_ == Capacity) {
            slots_[tail_] = value_copy;
            head_ = nextIndex(head_);
            tail_ = nextIndex(tail_);
            incrementDroppedSaturated();

            result.status = QueuePushStatus::ENQUEUED_DROPPED_OLDEST;
            result.dropped_oldest = true;
            result.size_after = count_;
            result.dropped_count_after = dropped_count_;
            return result;
        }

        slots_[tail_] = value_copy;
        tail_ = nextIndex(tail_);
        ++count_;

        result.status = QueuePushStatus::ENQUEUED;
        result.dropped_oldest = false;
        result.size_after = count_;
        result.dropped_count_after = dropped_count_;
        return result;
    }

    // Non-consuming inspection of oldest element. Returns nullptr if empty.
    // Returned pointer is invalidated by any subsequent `push()`, `pop()`, `clear()`, or destruction.
    const T* peek() const noexcept {
        if (count_ == 0U) {
            return nullptr;
        }
        return &slots_[head_];
    }

    // Alias for `peek()`; never consumes the head element.
    const T* front() const noexcept { return peek(); }

    // Non-consuming value-copy peek. Returns false if empty or `out_item == nullptr`.
    bool peek(T* out_item) const {
        if (out_item == nullptr || count_ == 0U) {
            return false;
        }
        *out_item = slots_[head_];
        return true;
    }

    // Removes the oldest element after caller confirms processing/ACK. Returns false when empty.
    bool pop() {
        if (count_ == 0U) {
            return false;
        }
        slots_[head_] = default_slot_;
        head_ = nextIndex(head_);
        --count_;
        return true;
    }

    // Copies and removes the oldest element. Returns false when empty or `out_item == nullptr`.
    bool pop(T* out_item) {
        if (out_item == nullptr || count_ == 0U) {
            return false;
        }
        *out_item = slots_[head_];
        slots_[head_] = default_slot_;
        head_ = nextIndex(head_);
        --count_;
        return true;
    }

    // Removes all buffered elements and resets ring indices while PRESERVING `dropped_count()`.
    void clear() {
        for (std::size_t i = 0U; i < Capacity; ++i) {
            slots_[i] = default_slot_;
        }
        head_ = 0U;
        tail_ = 0U;
        count_ = 0U;
    }

    // Explicitly resets cumulative `dropped_count()` without altering buffered items.
    void reset_dropped_count() noexcept { dropped_count_ = 0U; }

private:
    T default_slot_{};
    std::array<T, Capacity> slots_{};
    std::size_t head_ = 0U;
    std::size_t tail_ = 0U;
    std::size_t count_ = 0U;
    CounterT dropped_count_ = 0U;

    static constexpr std::size_t nextIndex(std::size_t idx) noexcept {
        return (idx + 1U == Capacity) ? 0U : (idx + 1U);
    }

    void incrementDroppedSaturated() noexcept {
        if (dropped_count_ < std::numeric_limits<CounterT>::max()) {
            ++dropped_count_;
        }
    }
};

}  // namespace node_env
