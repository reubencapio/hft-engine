#pragma once

/// @file matching_engine.hpp
/// @brief Order matching engine — processes incoming orders against the book.
///
/// Two operation modes:
///
///   Backtest mode (default constructor, synchronous):
///     Call process_order(order) → returns trades generated.
///     No threads, no queues. Deterministic.
///
///   Real-time mode (queue constructor, threaded):
///     Call start(cpu_core) to pin and run the engine thread.
///     Engine reads from input_queue_, pushes trades to output_queue_.
///     Call stop() to join the thread.
///
/// Matching logic (price-time priority, simplified aggregate levels):
///   - Limit buy:  match against asks at price <= order.price; post remainder
///   - Limit sell: match against bids at price >= order.price; post remainder
///   - Market buy: match against all available asks regardless of price
///   - Market sell: match against all available bids regardless of price
///   - Cancel:     remove quantity from the specified side at order.price

#include <atomic>
#include <thread>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"
#include "../metrics/latency_histogram.hpp"

namespace hft {

class MatchingEngine {
public:
    // ── Backtest mode constructor ──────────────────────────────────────────
    MatchingEngine() noexcept = default;

    // ── Real-time mode constructor ─────────────────────────────────────────
    /// @param input   Incoming order queue (shared with feed/strategy producer).
    /// @param output  Outgoing trade queue (shared with strategy consumer).
    MatchingEngine(SPSCQueue<Order, 65536>* input,
                   SPSCQueue<Trade, 65536>* output) noexcept;

    ~MatchingEngine();

    MatchingEngine(const MatchingEngine&)            = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // ── Real-time mode ─────────────────────────────────────────────────────

    /// @brief Start the engine thread and optionally pin it to a CPU core.
    /// @param cpu_core  0-based core index, or -1 for no pinning.
    void start(int cpu_core = -1);

    /// @brief Signal the engine thread to exit and wait for it to join.
    void stop() noexcept;

    // ── Backtest mode ──────────────────────────────────────────────────────

    /// @brief Synchronously process one order, returning all trades produced.
    /// Records RDTSC latency for each call. Not thread-safe — single-threaded
    /// use only (or external synchronization).
    [[nodiscard]] std::vector<Trade> process_order(Order order) noexcept;

    // ── Shared accessors ───────────────────────────────────────────────────

    [[nodiscard]] const OrderBook&        book()              const noexcept { return book_; }
    [[nodiscard]] const LatencyHistogram& latency_histogram() const noexcept { return latency_hist_; }

    /// @brief Reset order book and latency histogram for a fresh backtest run.
    void reset() noexcept;

private:
    /// @brief Core matching algorithm — called by both process_order and run_loop.
    /// Mutates order.quantity in-place on partial fills.
    [[nodiscard]] std::vector<Trade> do_match(Order& order) noexcept;

    /// @brief Engine thread body (real-time mode only).
    void run_loop() noexcept;

    // ── State ──────────────────────────────────────────────────────────────

    OrderBook        book_;
    LatencyHistogram latency_hist_;

    // ── Real-time queue pointers (null in backtest mode) ───────────────────

    SPSCQueue<Order, 65536>* input_queue_  = nullptr;
    SPSCQueue<Trade, 65536>* output_queue_ = nullptr;

    std::thread       engine_thread_;
    std::atomic<bool> running_{false};
};

} // namespace hft
