/// @file feed_simulator.cpp
/// @brief Market data feed replay simulator implementation.
///
/// Threading and timing details:
///
/// We use clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...) rather than
/// usleep() or nanosleep() with relative times because:
///   1. Absolute time avoids cumulative drift: if we oversleep on one
///      message, the next message's target time is still correct.
///   2. CLOCK_MONOTONIC is not affected by NTP adjustments.
///   3. On modern Linux (5.x+), the timer granularity is ~50ns with
///      CONFIG_HIGH_RES_TIMERS enabled.
///
/// Back-pressure: if the SPSC queue is full, we spin-wait with a pause
/// instruction rather than sleeping. This keeps the producer thread warm
/// and avoids the ~50µs wakeup latency of sleeping. The assumption is
/// that back-pressure is transient (consumer will drain quickly).
///
/// Jitter: optional random noise added to the sleep target for stress
/// testing. Uses a simple xorshift64 PRNG for deterministic, fast random
/// generation without heap allocation.

#include "feed_simulator.hpp"

#include <ctime>       // clock_nanosleep, timespec, CLOCK_MONOTONIC, TIMER_ABSTIME
#include <immintrin.h> // _mm_pause
#include <cstdlib>     // for xorshift state seeding

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// xorshift64 — minimal fast PRNG for jitter
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// @brief xorshift64 PRNG. Period: 2^64 - 1.
/// Not cryptographically secure (not needed for jitter).
/// Zero heap allocation, no state beyond a single uint64_t.
struct Xorshift64 {
    uint64_t state;

    explicit Xorshift64(uint64_t seed) noexcept : state(seed ? seed : 1) {}

    uint64_t next() noexcept {
        uint64_t x = state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return x;
    }
};

/// @brief Get current CLOCK_MONOTONIC time as a timespec.
inline struct timespec clock_now() noexcept {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

/// @brief Convert a timespec to nanoseconds.
inline uint64_t ts_to_ns(const struct timespec& ts) noexcept {
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

/// @brief Convert nanoseconds to a timespec.
inline struct timespec ns_to_ts(uint64_t ns) noexcept {
    struct timespec ts{};
    ts.tv_sec  = static_cast<time_t>(ns / 1'000'000'000ULL);
    ts.tv_nsec = static_cast<long>(ns % 1'000'000'000ULL);
    return ts;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

FeedSimulator::FeedSimulator(const std::vector<Order>& orders,
                             SPSCQueue<Order, 65536>& queue,
                             double speed_multiplier,
                             uint64_t jitter_ns) noexcept
    : orders_(orders)
    , queue_(queue)
    , speed_multiplier_(speed_multiplier)
    , jitter_ns_(jitter_ns)
{}

FeedSimulator::~FeedSimulator() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void FeedSimulator::start() {
    // Guard against double-start
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return; // Already running
    }

    orders_sent_.store(0, std::memory_order_relaxed);
    thread_ = std::thread(&FeedSimulator::run, this);
}

void FeedSimulator::stop() {
    running_.store(false, std::memory_order_release);

    if (thread_.joinable()) {
        thread_.join();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Replay Loop
// ─────────────────────────────────────────────────────────────────────────────

void FeedSimulator::run() {
    if (orders_.empty()) {
        running_.store(false, std::memory_order_release);
        return;
    }

    // ── Setup ────────────────────────────────────────────────────────────

    Xorshift64 rng(0xDEADBEEFCAFE1234ULL);

    // Record the wall-clock start time and the first message's timestamp.
    // All subsequent sleeps are computed as offsets from these two anchors.
    const uint64_t feed_start_ns = orders_[0].timestamp_ns;
    const uint64_t wall_start_ns = ts_to_ns(clock_now());

    // ── Main loop ────────────────────────────────────────────────────────

    for (std::size_t i = 0; i < orders_.size(); ++i) {
        // Check for stop signal
        if (!running_.load(std::memory_order_relaxed)) {
            break;
        }

        const Order& order = orders_[i];

        // ── Compute sleep target ─────────────────────────────────────────
        //
        // delta_ns = how far this message is from the first message in
        //            feed time (nanoseconds).
        //
        // If speed_multiplier > 0, we scale the delta:
        //   target_wall = wall_start + delta / speed_multiplier
        //
        // speed_multiplier = 1.0  → real-time
        // speed_multiplier = 10.0 → 10× faster
        // speed_multiplier = 0.0  → no sleeping (max throughput)

        if (speed_multiplier_ > 0.0 && order.timestamp_ns >= feed_start_ns) {
            const uint64_t delta_ns = order.timestamp_ns - feed_start_ns;
            const uint64_t scaled_delta =
                static_cast<uint64_t>(static_cast<double>(delta_ns) / speed_multiplier_);

            uint64_t target_ns = wall_start_ns + scaled_delta;

            // Optionally add jitter
            if (jitter_ns_ > 0) {
                const uint64_t jitter = rng.next() % (jitter_ns_ + 1);
                target_ns += jitter;
            }

            // Sleep until the target time. TIMER_ABSTIME means the kernel
            // will return immediately if the target is already in the past.
            struct timespec target_ts = ns_to_ts(target_ns);
            ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target_ts, nullptr);
        }

        // ── Push to queue ────────────────────────────────────────────────
        //
        // If the queue is full, spin-wait with a pause instruction. This
        // is preferable to sleeping because:
        //   1. The consumer (matching engine) should drain fast (<1µs).
        //   2. Sleeping would add ~50µs wakeup latency.
        //   3. _mm_pause() hints the CPU to save power during the spin.

        while (!queue_.push(order)) {
            // Check for stop signal even while spinning
            if (!running_.load(std::memory_order_relaxed)) {
                goto done;
            }
            _mm_pause();
        }

        orders_sent_.fetch_add(1, std::memory_order_relaxed);
    }

done:
    running_.store(false, std::memory_order_release);
}

} // namespace hft
