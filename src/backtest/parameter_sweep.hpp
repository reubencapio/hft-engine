#pragma once

/// @file parameter_sweep.hpp
/// @brief OpenMP-parallel parameter sweep for HFT backtesting.
///
/// The ParameterSweep class generates a Cartesian-product grid of
/// ParameterSet values and runs each configuration through a BacktestEngine
/// in parallel using OpenMP.
///
/// Thread-safety model:
///   - Each OpenMP thread owns a thread_local BacktestEngine instance so
///     there is ZERO shared mutable state between threads.
///   - The only shared data is the immutable tick_data vector and the
///     immutable parameter grid (both const).
///   - Results are written into a pre-sized vector at per-index offsets,
///     so no synchronization is needed.
///
/// Memory model:
///   - The grid is generated once and stored in a contiguous vector.
///   - Results are pre-allocated to avoid heap allocation during the
///     parallel loop.

#include <vector>

#include "backtest_engine.hpp"

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// SweepResult
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Pairs a ParameterSet with its BacktestResult for ranking.
///
/// Trivially copyable for MPI serialization (Phase 6).
struct SweepResult {
    ParameterSet  params;  ///< The parameter configuration that was tested
    BacktestResult result; ///< Resulting performance metrics

    /// @brief Compare by Sharpe ratio (descending) for sorting.
    bool operator>(const SweepResult& rhs) const noexcept {
        return result.sharpe > rhs.result.sharpe;
    }
};

static_assert(__is_trivially_copyable(SweepResult),
              "SweepResult must be trivially copyable for MPI serialization");

// ─────────────────────────────────────────────────────────────────────────────
// ParameterSweep
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Generates and evaluates a parameter grid using OpenMP parallelism.
///
/// Usage:
/// @code
///   ParameterSweep sweep;
///   auto results = sweep.run(tick_data);
///   // results[0] has the best Sharpe ratio
/// @endcode
///
/// Grid dimensions (configurable via set_*_range methods):
///   spread_ticks:  [min_spread, max_spread] with spread_step
///   order_qty:     [min_qty, max_qty] with qty_step
///   lookback_ns:   [min_lookback, max_lookback] with lookback_step
///   threshold_bps: [min_threshold, max_threshold] with threshold_step
///   max_position:  fixed value (not swept)
///   strategy_name: fixed value (not swept)
class ParameterSweep {
public:
    // ── Grid range configuration ────────────────────────────────────────

    /// @brief Set the range for spread_ticks.
    /// @param min_val  Minimum spread in ticks (inclusive).
    /// @param max_val  Maximum spread in ticks (inclusive).
    /// @param step     Step size between values.
    void set_spread_range(int32_t min_val, int32_t max_val, int32_t step) noexcept;

    /// @brief Set the range for order_qty.
    void set_qty_range(int32_t min_val, int32_t max_val, int32_t step) noexcept;

    /// @brief Set the range for lookback_ns.
    void set_lookback_range(uint64_t min_val, uint64_t max_val, uint64_t step) noexcept;

    /// @brief Set the range for threshold_bps.
    void set_threshold_range(int32_t min_val, int32_t max_val, int32_t step) noexcept;

    /// @brief Set fixed max_position for all grid points.
    void set_max_position(int32_t value) noexcept { max_position_ = value; }

    /// @brief Set which strategy to use for all grid points.
    void set_strategy(StrategyType strategy) noexcept { strategy_ = strategy; }

    // ── Grid generation ─────────────────────────────────────────────────

    /// @brief Build the full Cartesian product of all parameter ranges.
    ///
    /// Example: 50 spreads × 20 qtys × 10 lookbacks × 1 threshold = 10,000
    /// combinations.  Each combination becomes one ParameterSet.
    ///
    /// @return  Vector of all ParameterSet grid points.
    [[nodiscard]] std::vector<ParameterSet> generate_grid() const;

    // ── Execution ───────────────────────────────────────────────────────

    /// @brief Run the full parameter sweep in parallel.
    ///
    /// 1. Generates the grid via generate_grid().
    /// 2. Pre-allocates the results vector.
    /// 3. Runs an OpenMP parallel-for loop with dynamic scheduling.
    /// 4. Each thread uses a thread_local BacktestEngine.
    /// 5. Sorts results by Sharpe ratio descending.
    ///
    /// @param tick_data  Immutable market tick data (shared across threads).
    /// @return           Sorted vector of SweepResult (best Sharpe first).
    [[nodiscard]] std::vector<SweepResult> run(const std::vector<Order>& tick_data) const;

    /// @brief Run the sweep on a pre-built grid subset (used by MPI ranks).
    ///
    /// @param tick_data  Immutable market tick data.
    /// @param grid       Subset of parameter grid to evaluate.
    /// @return           Sorted vector of SweepResult (best Sharpe first).
    [[nodiscard]] std::vector<SweepResult> run_subset(
        const std::vector<Order>& tick_data,
        const std::vector<ParameterSet>& grid) const;

private:
    // ── Spread range ────────────────────────────────────────────────────
    int32_t min_spread_ = 2;
    int32_t max_spread_ = 50;
    int32_t spread_step_ = 2;

    // ── Quantity range ──────────────────────────────────────────────────
    int32_t min_qty_ = 100;
    int32_t max_qty_ = 2000;
    int32_t qty_step_ = 100;

    // ── Lookback range ──────────────────────────────────────────────────
    uint64_t min_lookback_ = 100'000'000ULL;      // 100ms
    uint64_t max_lookback_ = 1'000'000'000ULL;     // 1s
    uint64_t lookback_step_ = 100'000'000ULL;      // 100ms steps

    // ── Threshold range ─────────────────────────────────────────────────
    int32_t min_threshold_ = 5;
    int32_t max_threshold_ = 5;     // Default: single value (not swept)
    int32_t threshold_step_ = 5;

    // ── Fixed parameters ────────────────────────────────────────────────
    int32_t      max_position_ = 1000;
    StrategyType strategy_     = StrategyType::MarketMaker;
};

} // namespace hft
