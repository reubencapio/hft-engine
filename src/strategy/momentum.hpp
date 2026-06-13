#pragma once

/// @file momentum.hpp
/// @brief Momentum / trend-following strategy using mid-price velocity.
///
/// Strategy logic:
///   1. On each book update, sample the mid-price with an RDTSC timestamp.
///   2. Maintain a circular buffer of recent mid-price samples.
///   3. Compute the price change over the lookback window (in basis points).
///   4. If the change exceeds +threshold_bps → send a buy market order.
///      If the change exceeds -threshold_bps → send a sell market order.
///   5. Only one position at a time (flat-position constraint).
///
/// Parameters:
///   lookback_ns    — Time window in nanoseconds to measure price change.
///                    Default: 100,000 ns (100 µs). Short lookback captures
///                    microstructure momentum (order flow imbalance).
///   threshold_bps  — Minimum price change in basis points to trigger a
///                    signal. Default: 5 bps. Lower = more signals + noise.
///
/// RDTSC usage:
///   We use RDTSC (Read Time-Stamp Counter) for timestamping samples
///   because:
///     - It's ~5 ns to read vs ~25 ns for clock_gettime(CLOCK_MONOTONIC)
///     - We only need relative time differences, not absolute wall time
///     - On modern x86, RDTSC is invariant (constant rate regardless of
///       CPU frequency scaling)
///   We convert RDTSC ticks to nanoseconds using a calibrated ticks_per_ns
///   value computed at startup.
///
/// Circular buffer:
///   Fixed-size array of (rdtsc_tick, mid_price) pairs. Oldest entries
///   are overwritten. No heap allocation during operation.

#include "strategy_base.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace hft {

/// @brief A single mid-price sample with timestamp.
struct alignas(16) PriceSample {
    uint64_t rdtsc_tick = 0;   ///< RDTSC timestamp when sampled
    int64_t  mid_price  = 0;   ///< Mid-price at that instant (fixed-point)
};

/// @brief Momentum strategy that trades on short-term price velocity.
class MomentumStrategy : public StrategyBase {
public:
    /// @brief Construct with configurable parameters.
    /// @param lookback_ns    Lookback window in nanoseconds.
    /// @param threshold_bps  Minimum signal threshold in basis points.
    /// @param order_qty      Shares per market order signal.
    explicit MomentumStrategy(uint64_t lookback_ns = 100'000,
                              int64_t threshold_bps = 5,
                              int64_t order_qty = 100) noexcept;

    ~MomentumStrategy() override = default;

    // ─── StrategyBase interface ──────────────────────────────────────────

    void on_trade(const Trade& trade) override;
    void on_book_update(const OrderBook& book) override;
    void on_timer(uint64_t timestamp_ns) override;
    [[nodiscard]] std::string name() const override;

private:
    /// @brief Read the CPU Time-Stamp Counter.
    ///
    /// Uses RDTSCP which also serializes — ensures the timestamp reflects
    /// the actual execution point (not reordered by the CPU pipeline).
    /// The aux value (processor ID) is discarded.
    static uint64_t rdtsc() noexcept;

    /// @brief Calibrate RDTSC ticks-per-nanosecond by measuring against
    ///        CLOCK_MONOTONIC over a short interval.
    /// @return Ticks per nanosecond (typically 2-4 on modern CPUs).
    static double calibrate_ticks_per_ns() noexcept;

    /// @brief Find the oldest sample within the lookback window.
    /// @param current_tick  Current RDTSC value.
    /// @return Pointer to the sample, or nullptr if no sample is within range.
    ///
    /// Walks backward through the circular buffer to find the oldest sample
    /// whose timestamp is within [current - lookback_ticks, current].
    const PriceSample* find_lookback_sample(uint64_t current_tick) const noexcept;

    /// @brief Send a market order.
    /// @param side  Buy or Sell.
    /// @param qty   Number of shares.
    void send_market_order(Side side, int64_t qty) noexcept;

    // ─── Parameters ──────────────────────────────────────────────────────

    uint64_t lookback_ns_;       ///< Lookback window (ns)
    int64_t  threshold_bps_;     ///< Signal threshold (basis points)
    int64_t  order_qty_;         ///< Shares per signal order

    // ─── Derived constants ───────────────────────────────────────────────

    double   ticks_per_ns_;      ///< RDTSC ticks per nanosecond
    uint64_t lookback_ticks_;    ///< Lookback in RDTSC ticks

    // ─── Circular buffer of price samples ────────────────────────────────
    //
    // 4096 samples is sufficient for 100µs lookback at ~25ns per sample
    // (4096 × 25ns = 102.4µs of history).

    static constexpr std::size_t kMaxSamples = 4096;

    std::array<PriceSample, kMaxSamples> samples_ = {};
    std::size_t write_idx_   = 0;    ///< Next write position
    std::size_t num_samples_ = 0;    ///< Total samples stored (≤ kMaxSamples)

    // ─── Position state ──────────────────────────────────────────────────

    bool     has_position_   = false;   ///< True if we hold an open position
    uint64_t next_order_id_  = 1;       ///< Monotonic order ID counter
    uint64_t open_order_id_  = 0;       ///< ID of our open order (0 = none)
};

} // namespace hft
