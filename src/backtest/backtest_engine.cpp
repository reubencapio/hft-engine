/// @file backtest_engine.cpp
/// @brief Sequential backtesting engine implementation.

#include "backtest_engine.hpp"

#include <memory>

#include "../core/matching_engine.hpp"
#include "../strategy/market_maker.hpp"
#include "../strategy/momentum.hpp"

namespace hft {

BacktestEngine::BacktestEngine(const std::vector<Order>& tick_data) noexcept
    : tick_data_(tick_data) {}

BacktestResult BacktestEngine::run(const ParameterSet& params) const {
    // Create fresh state — run() is const so tick_data_ is never mutated.
    MatchingEngine            engine;
    SPSCQueue<Order, 65536>   strategy_orders;
    PnLTracker                pnl;

    // Instantiate the selected strategy.
    std::unique_ptr<StrategyBase> strategy;
    if (params.strategy_name == StrategyType::MarketMaker) {
        strategy = std::make_unique<MarketMaker>(
            static_cast<int64_t>(params.spread_ticks),
            static_cast<int64_t>(params.order_qty),
            static_cast<int64_t>(params.max_position));
    } else {
        strategy = std::make_unique<MomentumStrategy>(
            params.lookback_ns,
            static_cast<int64_t>(params.threshold_bps),
            static_cast<int64_t>(params.order_qty));
    }
    strategy->order_queue = &strategy_orders;
    strategy->pnl         = &pnl;

    uint64_t last_sample_ns = 0;

    for (const auto& tick : tick_data_) {
        // 1. Feed market data tick into the matching engine.
        auto mkt_trades = engine.process_order(tick);

        // 2. Notify strategy of book change.
        strategy->on_book_update(engine.book());

        // 3. Notify strategy of any market trades (for state tracking).
        for (const auto& t : mkt_trades) {
            strategy->on_trade(t);
        }

        // 4. Process orders the strategy submitted in response.
        Order strat_order;
        while (strategy_orders.pop(strat_order)) {
            auto strat_trades = engine.process_order(strat_order);
            for (const auto& st : strat_trades) {
                strategy->on_trade(st);
            }
        }

        // 5. Mark-to-market for unrealized P&L.
        if (const int64_t mid = engine.book().mid_price(); mid > 0) {
            pnl.mark_to_market(mid);
        }

        // 6. Per-second P&L snapshot for Sharpe calculation.
        const uint64_t tick_ns = tick.timestamp_ns;
        if (last_sample_ns == 0) {
            last_sample_ns = tick_ns;
        } else if (tick_ns > last_sample_ns + 1'000'000'000ULL) {
            pnl.record_pnl_sample(pnl.total_pnl());
            last_sample_ns = tick_ns;
        }
    }

    BacktestResult result;
    result.total_pnl    = static_cast<double>(pnl.total_pnl());
    result.sharpe       = pnl.sharpe_ratio();
    result.max_drawdown = static_cast<double>(pnl.max_drawdown());
    result.fill_rate    = (pnl.num_orders() > 0)
        ? static_cast<double>(pnl.num_trades()) /
          static_cast<double>(pnl.num_orders())
        : 0.0;
    result.avg_latency_ns = engine.latency_histogram().percentile(50.0);

    return result;
}

} // namespace hft
