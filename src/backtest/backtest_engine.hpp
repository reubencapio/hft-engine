#pragma once

/// @file backtest_engine.hpp
/// @brief Single-threaded backtesting engine for HFT strategy evaluation.
///
/// The BacktestEngine processes a pre-parsed vector of market ticks through a
/// full matching engine simulation, running a strategy and collecting metrics.
///
/// Design goals:
///   - Stateless between runs: each call to run() builds fresh internal state
///   - No heap allocation on hot paths (OrderBook uses pooled storage)
///   - Deterministic: sequential tick processing for reproducible results
///   - Thread-safe by isolation: OpenMP threads use thread_local instances
///
/// The engine is deliberately copyable — it only holds a const reference to
/// the shared tick data vector (immutable, never mutated during the sweep).

#include <cstdint>
#include <vector>

#include "../core/order.hpp"

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// Strategy enumeration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Selects which built-in strategy to instantiate during a backtest.
///
/// MarketMaker: two-sided quoting around mid, constrained by max_position.
/// Momentum:    trend-following based on recent price changes vs threshold_bps.
enum class StrategyType : uint8_t {
    MarketMaker = 0,
    Momentum    = 1
};

// ─────────────────────────────────────────────────────────────────────────────
// ParameterSet
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A single point in the parameter search space.
///
/// All fields are fixed-size integer types for:
///   1. Trivial serialization across MPI (MPI_Type_create_struct on POD).
///   2. Cache-friendly packing (the struct fits in 32 bytes + padding).
///   3. No heap allocation — no std::string, no std::vector.
///
/// Interpretation of each field depends on the strategy:
///   - MarketMaker uses spread_ticks, order_qty, max_position.
///   - Momentum uses lookback_ns, threshold_bps, order_qty.
///   - All strategies respect max_position as a risk limit.
struct ParameterSet {
    int32_t      spread_ticks  = 2;                    ///< Half-spread width in ticks (MarketMaker)
    int32_t      order_qty     = 100;                  ///< Order quantity per quote/signal
    uint64_t     lookback_ns   = 1'000'000'000ULL;     ///< Lookback window in nanoseconds (Momentum)
    int32_t      threshold_bps = 10;                   ///< Entry threshold in basis points (Momentum)
    int32_t      max_position  = 1000;                 ///< Maximum net position (risk limit)
    StrategyType strategy_name = StrategyType::MarketMaker; ///< Which strategy to run

    // 2 bytes padding implicit from enum + alignment
};

// Ensure ParameterSet is trivially copyable for MPI raw-byte transfer.
static_assert(__is_trivially_copyable(ParameterSet),
              "ParameterSet must be trivially copyable for MPI serialization");

// ─────────────────────────────────────────────────────────────────────────────
// BacktestResult
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Output metrics from a single backtest run.
///
/// All double fields; trivially copyable for MPI_Gather aggregation.
/// These are computed from the PnLTracker and LatencyHistogram after
/// the tick replay completes.
struct BacktestResult {
    double sharpe         = 0.0;   ///< Annualized Sharpe ratio (√252 scaling)
    double max_drawdown   = 0.0;   ///< Maximum peak-to-trough drawdown (absolute)
    double total_pnl      = 0.0;   ///< Final realized + unrealized P&L
    double fill_rate      = 0.0;   ///< Fraction of orders that were filled [0, 1]
    double avg_latency_ns = 0.0;   ///< Mean order-to-fill latency (nanoseconds)
};

static_assert(__is_trivially_copyable(BacktestResult),
              "BacktestResult must be trivially copyable for MPI serialization");

// ─────────────────────────────────────────────────────────────────────────────
// BacktestEngine
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Sequential, deterministic backtesting engine.
///
/// Usage:
/// @code
///   const std::vector<Order>& ticks = load_itch("data.itch");
///   BacktestEngine engine(ticks);
///   BacktestResult r = engine.run(params);
/// @endcode
///
/// Thread-safety model:
///   - The tick_data reference is immutable — safe to share across threads.
///   - Each thread must use its own BacktestEngine instance (thread_local).
///   - run() creates fresh OrderBook/MatchingEngine/Strategy each invocation.
class BacktestEngine {
public:
    /// @brief Construct with a reference to pre-parsed market tick data.
    ///
    /// @param tick_data  Immutable vector of Order ticks.  The vector must
    ///                   outlive the engine (typically a global or sweep-scope
    ///                   variable).  The engine does NOT copy the data.
    explicit BacktestEngine(const std::vector<Order>& tick_data) noexcept;

    /// @brief Run a full backtest with the given parameter configuration.
    ///
    /// Creates a fresh OrderBook, MatchingEngine, and Strategy internally.
    /// Iterates through every tick in tick_data sequentially, feeding them
    /// into the matching engine and strategy callbacks.
    ///
    /// @param params  The strategy parameters for this run.
    /// @return        Aggregated result metrics.
    BacktestResult run(const ParameterSet& params) const;

    /// @brief Number of ticks in the underlying data set (for diagnostics).
    [[nodiscard]] std::size_t tick_count() const noexcept { return tick_data_.size(); }

private:
    /// @brief Reference to shared, immutable tick data.
    ///
    /// This is the ONLY state the engine holds.  Because it's a const ref,
    /// multiple threads can safely read from the same underlying vector.
    const std::vector<Order>& tick_data_;
};

} // namespace hft
