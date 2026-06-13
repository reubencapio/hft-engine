#pragma once

/// @file market_maker.hpp
/// @brief Market-making strategy — posts bid/ask quotes around the spread.
///
/// Strategy logic:
///   1. On each book update, if we have no open orders:
///      - Post a bid at best_bid - spread_ticks/2
///      - Post an ask at best_ask + spread_ticks/2
///   2. On fill:
///      - Update position via PnLTracker.
///      - If abs(position) exceeds max_position, send a market order to
///        flatten back to zero (risk management).
///   3. Track order count and P&L via the injected PnLTracker.
///
/// Parameters:
///   spread_ticks  — Distance from best bid/ask to our quotes (in price ticks).
///                   Default: 2. A larger spread means less adverse selection
///                   risk but lower fill probability.
///   order_qty     — Size of each posted order. Default: 100 shares.
///   max_position  — Absolute position limit. Exceeding this triggers a
///                   market order to flatten. Default: 1000 shares.
///
/// Simplifications for backtesting:
///   - We track "has_open_orders" locally as a boolean rather than
///     maintaining a full order-to-fill mapping. This is sufficient
///     because the strategy only posts one bid and one ask at a time.
///   - Order IDs use a simple incrementing counter.

#include "strategy_base.hpp"

#include <cstdint>
#include <cstdlib>   // std::abs
#include <string>

namespace hft {

/// @brief Market-making strategy implementation.
class MarketMaker : public StrategyBase {
public:
    /// @brief Construct with configurable parameters.
    /// @param spread_ticks   Number of ticks between our quote and the BBO.
    /// @param order_qty      Shares per order.
    /// @param max_position   Maximum absolute position before flattening.
    explicit MarketMaker(int64_t spread_ticks = 2,
                         int64_t order_qty = 100,
                         int64_t max_position = 1000) noexcept;

    ~MarketMaker() override = default;

    // ─── StrategyBase interface ──────────────────────────────────────────

    void on_trade(const Trade& trade) override;
    void on_book_update(const OrderBook& book) override;
    void on_timer(uint64_t timestamp_ns) override;
    [[nodiscard]] std::string name() const override;

private:
    /// @brief Send a limit order through the SPSC queue.
    /// @param side   Buy or Sell.
    /// @param price  Limit price (fixed-point).
    /// @param qty    Order quantity.
    void send_limit_order(Side side, int64_t price, int64_t qty) noexcept;

    /// @brief Send a market order to flatten the current position.
    void flatten_position() noexcept;

    // ─── Parameters ──────────────────────────────────────────────────────

    int64_t spread_ticks_;    ///< Half-spread in tick units
    int64_t order_qty_;       ///< Shares per quote
    int64_t max_position_;    ///< Absolute position limit

    // ─── State ───────────────────────────────────────────────────────────

    bool     has_open_orders_ = false;   ///< True if we have resting orders
    uint64_t next_order_id_   = 1;       ///< Monotonic order ID counter
    uint64_t bid_order_id_    = 0;       ///< ID of our current resting bid (0 = none)
    uint64_t ask_order_id_    = 0;       ///< ID of our current resting ask (0 = none)
};

} // namespace hft
