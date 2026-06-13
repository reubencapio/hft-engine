#pragma once

/// @file feed_simulator.hpp
/// @brief Market data feed replay simulator.
///
/// The FeedSimulator takes a pre-parsed vector of Orders (typically from
/// ITCHParser::parse_all()) and replays them at a configurable speed into
/// an SPSC queue that the matching engine consumes.
///
/// Timing model:
///   - The simulator runs on its own dedicated thread.
///   - It uses clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) for precise
///     inter-message timing. Absolute-time sleeping avoids drift accumulation
///     that relative sleeps would cause.
///   - The speed_multiplier_ controls replay speed:
///       1.0 = real-time, 10.0 = 10× faster, 0.0 = unlimited (no sleep)
///   - Optional jitter_ns adds random nanosecond noise to each sleep for
///     stress testing timing-sensitive strategies.
///
/// Thread model:
///   - start() spawns the replay thread.
///   - stop() signals and joins.
///   - The thread pushes Orders into the SPSC queue; if the queue is full,
///     it spin-waits with a brief pause (to avoid overwhelming a slow consumer).
///
/// This runs on the "warm" path — it's not the matching engine hot path,
/// but we still avoid heap allocation during replay.

#include "../core/order.hpp"
#include "../core/spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace hft {

/// @brief Replays a sequence of Orders into an SPSC queue with timing control.
class FeedSimulator {
public:
    /// @brief Construct a feed simulator.
    /// @param orders          Pre-parsed order messages to replay.
    /// @param queue           SPSC queue shared with the matching engine consumer.
    /// @param speed_multiplier  Replay speed (1.0 = real-time, 0.0 = max speed).
    /// @param jitter_ns       Maximum random jitter in nanoseconds (0 = none).
    FeedSimulator(const std::vector<Order>& orders,
                  SPSCQueue<Order, 65536>& queue,
                  double speed_multiplier = 1.0,
                  uint64_t jitter_ns = 0) noexcept;

    /// @brief Destructor — ensures the replay thread is stopped.
    ~FeedSimulator();

    // Non-copyable, non-movable (owns thread)
    FeedSimulator(const FeedSimulator&) = delete;
    FeedSimulator& operator=(const FeedSimulator&) = delete;

    /// @brief Spawn the replay thread and begin feeding orders.
    /// Does nothing if already running.
    void start();

    /// @brief Signal the replay thread to stop and wait for it to join.
    /// Safe to call multiple times or if never started.
    void stop();

    /// @brief Check if the simulator is currently running.
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

    /// @brief Number of orders that have been pushed so far.
    [[nodiscard]] uint64_t orders_sent() const noexcept {
        return orders_sent_.load(std::memory_order_relaxed);
    }

private:
    /// @brief Main replay loop executed on the simulator thread.
    ///
    /// Algorithm:
    ///   1. Record wall-clock start time and first message timestamp.
    ///   2. For each order:
    ///      a. Compute the target wall-clock time based on the message's
    ///         timestamp delta from the first message, scaled by speed_multiplier.
    ///      b. Optionally add jitter.
    ///      c. Sleep until the target time using TIMER_ABSTIME.
    ///      d. Push the order into the SPSC queue (spin if full).
    ///   3. Exit when all orders are sent or stop() is called.
    void run();

    // ─── Configuration ───────────────────────────────────────────────────

    const std::vector<Order>& orders_;              ///< Reference to order data
    SPSCQueue<Order, 65536>&  queue_;               ///< Output queue
    double                    speed_multiplier_;     ///< Replay speed factor
    uint64_t                  jitter_ns_;            ///< Max jitter (ns)

    // ─── Runtime state ───────────────────────────────────────────────────

    std::atomic<bool>     running_{false};           ///< Run flag (set by start/stop)
    std::atomic<uint64_t> orders_sent_{0};           ///< Progress counter
    std::thread           thread_;                   ///< Replay thread
};

} // namespace hft
