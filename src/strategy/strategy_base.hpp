#pragma once

/// @file strategy_base.hpp
/// @brief Abstract base class for all trading strategies.
///
/// All strategies in the backtesting engine implement this interface.
/// The matching engine / backtest harness calls these methods:
///
///   on_trade()       — when a trade occurs (execution report)
///   on_book_update() — when the order book changes (add/cancel/modify)
///   on_timer()       — periodic timer callbacks (e.g. every 100µs)
///
/// Strategies send orders by pushing to `order_queue` and track
/// performance via `pnl`.
///
/// Design decisions:
///   - Virtual dispatch overhead is acceptable here: strategies are on
///     the "warm" path (few µs per call), not the matching engine hot
///     path (sub-µs).
///   - The SPSC queue pointer is set externally by the backtest harness
///     so strategies don't manage queue lifetime.
///   - name() returns std::string for logging/reporting only — never
///     called on the hot path.

#include "../core/order.hpp"
#include "../core/order_book.hpp"
#include "../core/spsc_queue.hpp"
#include "../metrics/pnl_tracker.hpp"

#include <cstdint>
#include <string>

namespace hft {

/// @brief Abstract base class for trading strategies.
///
/// Concrete strategies must implement:
///   - on_trade():       react to trade executions
///   - on_book_update(): react to order book changes
///   - on_timer():       react to periodic timer events
///   - name():           return a human-readable strategy name
///
/// Example:
///   class MyStrategy : public StrategyBase {
///       void on_book_update(const OrderBook& book) override {
///           Order order{};
///           order.type = OrderType::Limit;
///           order.price = book.best_bid();
///           order.quantity = 100;
///           order.side = Side::Buy;
///           if (order_queue) order_queue->push(order);
///           if (pnl) pnl->on_order_sent();
///       }
///       // ... other overrides ...
///   };
class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    // ─── Event callbacks ─────────────────────────────────────────────────

    /// @brief Called when a trade execution occurs.
    /// @param trade  The completed trade.
    virtual void on_trade(const Trade& trade) = 0;

    /// @brief Called when the order book has been updated.
    /// @param book  The current state of the order book.
    ///
    /// This is the primary signal for most strategies. The book reflects
    /// the state AFTER the most recent add/cancel/modify.
    virtual void on_book_update(const OrderBook& book) = 0;

    /// @brief Called on periodic timer events.
    /// @param timestamp_ns  Current simulation time in nanoseconds.
    ///
    /// Useful for time-based decisions (e.g. cancel stale orders,
    /// compute TWAP slices, check timeout conditions).
    virtual void on_timer(uint64_t timestamp_ns) = 0;

    /// @brief Human-readable name for this strategy instance.
    /// Used in logging and performance reports. NOT called on hot path.
    [[nodiscard]] virtual std::string name() const = 0;

    // ─── Injected dependencies ───────────────────────────────────────────
    //
    // These are set by the backtest harness before the strategy receives
    // any callbacks. Raw pointers are used intentionally:
    //   - The queue and tracker outlive the strategy.
    //   - No ownership semantics needed.
    //   - Avoids shared_ptr overhead on the warm path.

    /// @brief Queue for submitting orders to the matching engine.
    /// Push orders here to place them into the book.
    SPSCQueue<Order, 65536>* order_queue = nullptr;

    /// @brief P&L and risk metric tracker for this strategy.
    PnLTracker* pnl = nullptr;
};

} // namespace hft
