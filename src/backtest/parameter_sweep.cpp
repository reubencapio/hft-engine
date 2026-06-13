/// @file parameter_sweep.cpp
/// @brief OpenMP-parallel parameter sweep implementation.

#include "parameter_sweep.hpp"

#include <algorithm>
#include <memory>

namespace hft {

// ─── Range setters ────────────────────────────────────────────────────────────

void ParameterSweep::set_spread_range(int32_t min_val, int32_t max_val, int32_t step) noexcept {
    min_spread_  = min_val;
    max_spread_  = max_val;
    spread_step_ = (step > 0) ? step : 1;
}

void ParameterSweep::set_qty_range(int32_t min_val, int32_t max_val, int32_t step) noexcept {
    min_qty_  = min_val;
    max_qty_  = max_val;
    qty_step_ = (step > 0) ? step : 1;
}

void ParameterSweep::set_lookback_range(uint64_t min_val, uint64_t max_val, uint64_t step) noexcept {
    min_lookback_  = min_val;
    max_lookback_  = max_val;
    lookback_step_ = (step > 0) ? step : 1;
}

void ParameterSweep::set_threshold_range(int32_t min_val, int32_t max_val, int32_t step) noexcept {
    min_threshold_  = min_val;
    max_threshold_  = max_val;
    threshold_step_ = (step > 0) ? step : 1;
}

// ─── Grid generation ──────────────────────────────────────────────────────────

std::vector<ParameterSet> ParameterSweep::generate_grid() const {
    std::vector<ParameterSet> grid;

    for (int32_t spread = min_spread_; spread <= max_spread_; spread += spread_step_) {
        for (int32_t qty = min_qty_; qty <= max_qty_; qty += qty_step_) {
            for (uint64_t lb = min_lookback_; lb <= max_lookback_; lb += lookback_step_) {
                for (int32_t thr = min_threshold_; thr <= max_threshold_; thr += threshold_step_) {
                    ParameterSet ps;
                    ps.spread_ticks  = spread;
                    ps.order_qty     = qty;
                    ps.lookback_ns   = lb;
                    ps.threshold_bps = thr;
                    ps.max_position  = max_position_;
                    ps.strategy_name = strategy_;
                    grid.push_back(ps);
                }
            }
        }
    }

    return grid;
}

// ─── Execution ────────────────────────────────────────────────────────────────

std::vector<SweepResult> ParameterSweep::run(const std::vector<Order>& tick_data) const {
    return run_subset(tick_data, generate_grid());
}

std::vector<SweepResult> ParameterSweep::run_subset(
    const std::vector<Order>& tick_data,
    const std::vector<ParameterSet>& grid) const
{
    std::vector<SweepResult> results(grid.size());

    // Each OpenMP thread uses a thread_local BacktestEngine so there is no
    // shared mutable state between threads. The engine is created once per
    // thread on first access and reused for all iterations on that thread.
#pragma omp parallel for schedule(dynamic, 16)
    for (int i = 0; i < static_cast<int>(grid.size()); ++i) {
        thread_local std::unique_ptr<BacktestEngine> tl_engine;
        thread_local const std::vector<Order>*       tl_data_ptr = nullptr;
        if (tl_data_ptr != &tick_data) {
            tl_engine    = std::make_unique<BacktestEngine>(tick_data);
            tl_data_ptr  = &tick_data;
        }
        results[i].params = grid[i];
        results[i].result = tl_engine->run(grid[i]);
    }

    std::sort(results.begin(), results.end(),
        [](const SweepResult& a, const SweepResult& b) {
            return a.result.sharpe > b.result.sharpe;
        });

    return results;
}

} // namespace hft
