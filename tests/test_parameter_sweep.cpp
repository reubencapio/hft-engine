/// @file test_parameter_sweep.cpp
/// @brief Google Test suite for the parameter sweep.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "backtest/backtest_engine.hpp"
#include "backtest/parameter_sweep.hpp"
#include "core/order.hpp"

using hft::Order;
using hft::OrderType;
using hft::ParameterSet;
using hft::ParameterSweep;
using hft::Side;
using hft::StrategyType;
using hft::SweepResult;

namespace {

// Build a minimal tick sequence: a few bids and asks with timestamps
std::vector<Order> make_ticks(int n = 100) {
    std::vector<Order> ticks;
    ticks.reserve(static_cast<std::size_t>(n * 2));

    int64_t  mid = 1'000'000;
    uint64_t ns  = 1'000'000'000ULL;
    uint64_t id  = 100;

    for (int i = 0; i < n; ++i) {
        Order bid{};
        bid.order_id     = id++;
        bid.order_type   = OrderType::Limit;
        bid.side         = Side::Bid;
        bid.price        = mid - 100;
        bid.quantity     = 100;
        bid.timestamp_ns = ns;
        ticks.push_back(bid);

        Order ask{};
        ask.order_id     = id++;
        ask.order_type   = OrderType::Limit;
        ask.side         = Side::Ask;
        ask.price        = mid + 100;
        ask.quantity     = 100;
        ask.timestamp_ns = ns;
        ticks.push_back(ask);

        mid += (i % 5 == 0) ? 50 : (i % 5 == 1 ? -50 : 0);
        ns  += 10'000'000ULL; // 10ms between ticks
    }
    return ticks;
}

} // anonymous namespace

// ─── BacktestEngine: basic smoke test ────────────────────────────────────────

TEST(BacktestEngine, SmokeTest) {
    auto ticks = make_ticks(50);
    hft::BacktestEngine engine(ticks);

    ParameterSet params;
    params.strategy_name = StrategyType::MarketMaker;
    params.spread_ticks  = 2;
    params.order_qty     = 100;
    params.max_position  = 1000;

    const auto result = engine.run(params);

    // Just verify the result is populated (no NaN, fill_rate in [0,1])
    EXPECT_GE(result.fill_rate, 0.0);
    EXPECT_LE(result.fill_rate, 1.0);
    EXPECT_GE(result.avg_latency_ns, 0.0);
}

TEST(BacktestEngine, MomentumSmokeTest) {
    auto ticks = make_ticks(200);
    hft::BacktestEngine engine(ticks);

    ParameterSet params;
    params.strategy_name = StrategyType::Momentum;
    params.lookback_ns   = 50'000'000ULL;  // 50ms
    params.threshold_bps = 3;
    params.order_qty     = 100;

    const auto result = engine.run(params);
    EXPECT_GE(result.fill_rate, 0.0);
    EXPECT_LE(result.fill_rate, 1.0);
}

// ─── ParameterSweep: generate_grid ───────────────────────────────────────────

TEST(ParameterSweep, GenerateGrid) {
    ParameterSweep sweep;
    sweep.set_spread_range(1, 3, 1);                                   // 3 values: 1, 2, 3
    sweep.set_qty_range(100, 200, 100);                                 // 2 values: 100, 200
    sweep.set_lookback_range(100'000'000ULL, 100'000'000ULL, 100'000'000ULL); // 1 value
    sweep.set_threshold_range(5, 5, 5);                                // 1 value

    const auto grid = sweep.generate_grid();
    EXPECT_EQ(grid.size(), 6u);  // 3 × 2 × 1 × 1 = 6
}

TEST(ParameterSweep, GridDefaultLookbackRange) {
    ParameterSweep sweep;
    sweep.set_spread_range(1, 2, 1);   // 2
    sweep.set_qty_range(100, 100, 100); // 1
    // Default lookback: 100ms to 1s step 100ms → 10 values
    // Default threshold: 5 to 5 step 5 → 1 value
    // Total: 2 × 1 × 10 × 1 = 20
    const auto grid = sweep.generate_grid();
    EXPECT_EQ(grid.size(), 20u);
}

// ─── ParameterSweep: run_subset ───────────────────────────────────────────────

TEST(ParameterSweep, RunSubsetReturnsSortedBySharpDesc) {
    auto ticks = make_ticks(100);

    ParameterSweep sweep;
    sweep.set_spread_range(1, 4, 1);
    sweep.set_qty_range(100, 200, 100);

    const auto results = sweep.run(ticks);

    ASSERT_FALSE(results.empty());

    // Verify descending Sharpe order
    for (std::size_t i = 1; i < results.size(); ++i) {
        EXPECT_GE(results[i - 1].result.sharpe, results[i].result.sharpe)
            << "results not sorted at index " << i;
    }
}

TEST(ParameterSweep, RunSubsetCorrectCount) {
    auto ticks = make_ticks(20);

    ParameterSweep sweep;
    sweep.set_spread_range(1, 3, 1);                                        // 3
    sweep.set_qty_range(100, 100, 100);                                     // 1
    sweep.set_lookback_range(100'000'000ULL, 100'000'000ULL, 100'000'000ULL); // 1
    sweep.set_threshold_range(5, 5, 5);                                     // 1

    const auto results = sweep.run(ticks);
    EXPECT_EQ(results.size(), 3u);
}

// ─── ParameterSweep: empty tick data ─────────────────────────────────────────

TEST(ParameterSweep, EmptyTickData) {
    std::vector<Order> empty_ticks;
    ParameterSweep sweep;
    sweep.set_spread_range(1, 2, 1);
    sweep.set_qty_range(100, 100, 100);
    sweep.set_lookback_range(100'000'000ULL, 100'000'000ULL, 100'000'000ULL); // 1
    sweep.set_threshold_range(5, 5, 5);                                       // 1

    const auto results = sweep.run(empty_ticks);
    EXPECT_EQ(results.size(), 2u);
    for (const auto& r : results) {
        EXPECT_EQ(r.result.total_pnl, 0.0);
        EXPECT_EQ(r.result.fill_rate, 0.0);
    }
}

// ─── Determinism: same params → same result ───────────────────────────────────

TEST(BacktestEngine, Deterministic) {
    auto ticks = make_ticks(100);

    ParameterSet params;
    params.strategy_name = StrategyType::MarketMaker;
    params.spread_ticks  = 3;
    params.order_qty     = 100;
    params.max_position  = 500;

    hft::BacktestEngine engine(ticks);
    const auto r1 = engine.run(params);
    const auto r2 = engine.run(params);

    EXPECT_EQ(r1.total_pnl,    r2.total_pnl);
    EXPECT_EQ(r1.sharpe,       r2.sharpe);
    EXPECT_EQ(r1.max_drawdown, r2.max_drawdown);
    EXPECT_EQ(r1.fill_rate,    r2.fill_rate);
}
