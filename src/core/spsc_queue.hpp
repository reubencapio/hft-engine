#pragma once

// =============================================================================
// spsc_queue.hpp — Lock-free Single-Producer Single-Consumer Ring Buffer
//
// This is the primary inter-thread communication primitive for the HFT engine.
// One thread pushes (producer), one thread pops (consumer). No locks, no CAS
// loops, no heap allocation after construction.
//
// Design decisions:
//   - Power-of-two capacity N enables bitwise AND masking instead of modulo.
//   - Head and tail are monotonically increasing uint64_t counters, each on
//     its own cache line to prevent false sharing between producer and consumer.
//   - Memory ordering: acquire on loads, release on stores. No seq_cst —
//     the SPSC pattern only needs store-release / load-acquire pairs.
//   - The ring buffer array is alignas(64) to keep the first element on a
//     cache-line boundary.
//   - No exceptions, no dynamic allocation. push/pop return bool.
//
// Correctness argument:
//   - Producer owns `tail_`, reads `head_` (acquire) to check space.
//   - Consumer owns `head_`, reads `tail_` (acquire) to check data.
//   - Data is written to buffer_[tail_ & mask] BEFORE tail_ is published
//     with release, so the consumer always sees fully written data.
//   - Symmetrically, the consumer reads buffer_[head_ & mask] BEFORE
//     publishing head_ with release, so the producer always sees a free slot.
// =============================================================================

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace hft {

/// @brief Lock-free SPSC ring buffer.
/// @tparam T    Element type. Must be trivially copyable (no destructors to
///              run when overwriting slots).
/// @tparam N    Capacity. Must be a power of two so we can use bitmask indexing.
template <typename T, std::size_t N>
class SPSCQueue {
    // Compile-time constraint: N must be a power of two for bitmask indexing.
    static_assert(N > 0 && (N & (N - 1)) == 0,
        "SPSCQueue capacity N must be a power of two");

    // T must be trivially copyable — we memcpy / assign into the ring buffer
    // and never call destructors on evicted slots.
    static_assert(std::is_trivially_copyable_v<T>,
        "SPSCQueue element type must be trivially copyable");

    // Bitmask for converting a monotonic counter to a ring-buffer index.
    // Because N is a power of two, (counter & kMask) == (counter % N) but
    // compiles to a single AND instruction instead of a division.
    static constexpr std::size_t kMask = N - 1;

public:
    SPSCQueue() noexcept = default;

    // Non-copyable, non-movable — the atomics have no meaningful copy semantics
    // and the buffer is embedded in-place.
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&)                 = delete;
    SPSCQueue& operator=(SPSCQueue&&)      = delete;

    // -----------------------------------------------------------------------
    // push — Enqueue one element (producer thread only)
    // -----------------------------------------------------------------------
    // Returns false if the queue is full (no blocking, no spinning).
    //
    // Memory ordering:
    //   1. Load head_ with acquire — synchronizes with the consumer's release
    //      store to head_, ensuring we see the consumer's latest progress.
    //   2. Write data into the buffer (plain store, sequenced before the
    //      release store below).
    //   3. Store tail_ with release — makes the buffer write visible to the
    //      consumer before it sees the updated tail.
    // -----------------------------------------------------------------------
    bool push(const T& item) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);

        // Full when the producer is exactly N slots ahead of the consumer.
        if (tail - head >= N) {
            return false;
        }

        buffer_[tail & kMask] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // pop — Dequeue one element (consumer thread only)
    // -----------------------------------------------------------------------
    // Returns false if the queue is empty (no blocking, no spinning).
    //
    // Memory ordering:
    //   1. Load tail_ with acquire — synchronizes with the producer's release
    //      store to tail_, ensuring we see the latest data.
    //   2. Read data from the buffer (plain load, sequenced before the
    //      release store below).
    //   3. Store head_ with release — makes the slot available to the
    //      producer for reuse.
    // -----------------------------------------------------------------------
    bool pop(T& item) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);

        // Empty when consumer has caught up with producer.
        if (head >= tail) {
            return false;
        }

        item = buffer_[head & kMask];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // Utility methods (not for hot-path use — these load both atomics)
    // -----------------------------------------------------------------------

    /// @brief Current number of items in the queue (approximate — racy read).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(tail - head);
    }

    /// @brief True if the queue appears empty (approximate — racy read).
    [[nodiscard]] bool empty_approx() const noexcept {
        return size_approx() == 0;
    }

    /// @brief Compile-time capacity.
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return N;
    }

private:
    // -----------------------------------------------------------------------
    // Data layout — cache-line separation of producer / consumer state
    // -----------------------------------------------------------------------
    // The head_ and tail_ atomics are each placed on their own 64-byte cache
    // line. This eliminates false sharing: the producer only writes tail_,
    // the consumer only writes head_. Without this separation, every push
    // would invalidate the consumer's cache line and vice versa.
    // -----------------------------------------------------------------------

    /// Producer's write cursor (monotonically increasing).
    /// Only written by the producer thread; read by the consumer.
    alignas(64) std::atomic<uint64_t> tail_{0};

    /// Consumer's read cursor (monotonically increasing).
    /// Only written by the consumer thread; read by the producer.
    alignas(64) std::atomic<uint64_t> head_{0};

    /// Ring buffer storage. alignas(64) ensures the array starts on a
    /// cache-line boundary for optimal prefetching.
    alignas(64) T buffer_[N] = {};
};

} // namespace hft
