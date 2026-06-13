#pragma once

/// @file pnl_tracker.hpp
/// @brief Header-only P&L tracking and risk metrics for strategies.
///
/// This tracker is designed to be called from the hot path with minimal
/// overhead. Key design decisions:
///
///   - All fields are fixed-size integers or doubles (no heap allocation).
///   - The per-second P&L samples for Sharpe ratio use a fixed-size
///     circular buffer to avoid dynamic allocation.
///   - Max drawdown is tracked incrementally (O(1) per update).
///   - The tracker does NOT take locks — it should be owned by a single
///     strategy thread.
///
/// Price representation: prices are int64_t in fixed-point format (same
/// as Order::price). P&L values are in the same units.
///
/// Sharpe ratio: computed over a rolling window of per-second P&L samples.
/// We use the standard formula: Sharpe = mean(returns) / stdev(returns)
/// annualized by sqrt(seconds_per_year). For intraday backtesting, the
/// annualization factor is optional and can be ignored.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace hft {

/// @brief Tracks P&L, position, and risk metrics for a single strategy.
///
/// Usage:
///   PnLTracker tracker;
///   tracker.on_fill(Side::Buy, 100, 1502500);   // bought 100 @ 150.25
///   tracker.on_fill(Side::Sell, 100, 1505000);   // sold 100 @ 150.50
///   double pnl = tracker.realized_pnl();         // +2500 (= 0.25 × 10000)
class PnLTracker {
public:
    // ─── Fill recording ──────────────────────────────────────────────────

    /// @brief Record a fill (execution).
    /// @param is_buy   True if this is a buy fill.
    /// @param qty      Number of shares filled (always positive).
    /// @param price    Execution price (fixed-point).
    ///
    /// P&L accounting model:
    ///   - We track average entry price and current position.
    ///   - When a fill reduces or flips position, realized P&L is booked
    ///     as (exit_price - avg_entry) × qty_closed for longs, or
    ///     (avg_entry - exit_price) × qty_closed for shorts.
    ///   - When a fill increases position, avg_entry is updated as a
    ///     weighted average.
    void on_fill(bool is_buy, int64_t qty, int64_t price) noexcept {
        num_trades_++;

        const int64_t signed_qty = is_buy ? qty : -qty;

        if (position_ == 0) {
            // Opening a new position — simple case
            position_ = signed_qty;
            avg_entry_price_ = price;
        } else if ((position_ > 0 && signed_qty > 0) ||
                   (position_ < 0 && signed_qty < 0)) {
            // Increasing existing position — update weighted average entry
            //
            // new_avg = (old_avg × old_pos + price × new_qty) / total_pos
            //
            // We use absolute values for the weighted average calculation
            // to avoid sign confusion.
            const int64_t abs_old = std::abs(position_);
            const int64_t abs_new = std::abs(signed_qty);
            avg_entry_price_ =
                (avg_entry_price_ * abs_old + price * abs_new) / (abs_old + abs_new);
            position_ += signed_qty;
        } else {
            // Reducing or flipping position — book realized P&L
            const int64_t close_qty = std::min(std::abs(signed_qty), std::abs(position_));

            if (position_ > 0) {
                // Closing long: profit = (exit - entry) × qty
                realized_pnl_ += (price - avg_entry_price_) * close_qty;
            } else {
                // Closing short: profit = (entry - exit) × qty
                realized_pnl_ += (avg_entry_price_ - price) * close_qty;
            }

            position_ += signed_qty;

            // If position flipped, the remainder opens at the new price
            if (position_ != 0 &&
                ((position_ > 0 && signed_qty > 0) == false)) {
                avg_entry_price_ = price;
            }
        }

        // Update running peak and drawdown
        update_drawdown();
    }

    // ─── Order tracking ──────────────────────────────────────────────────

    /// @brief Increment the order counter (called when a strategy sends an order).
    void on_order_sent() noexcept { num_orders_++; }

    // ─── Mark-to-market ──────────────────────────────────────────────────

    /// @brief Update the last known price for unrealized P&L calculation.
    /// @param price  Current market price (e.g. mid-price from order book).
    void mark_to_market(int64_t price) noexcept {
        last_price_ = price;
    }

    // ─── Accessors ───────────────────────────────────────────────────────

    /// @brief Realized P&L from closed positions (fixed-point units).
    [[nodiscard]] int64_t realized_pnl() const noexcept {
        return realized_pnl_;
    }

    /// @brief Unrealized P&L from open position marked to last price.
    [[nodiscard]] int64_t unrealized_pnl() const noexcept {
        if (position_ == 0 || last_price_ == 0) return 0;

        if (position_ > 0) {
            return (last_price_ - avg_entry_price_) * position_;
        } else {
            return (avg_entry_price_ - last_price_) * (-position_);
        }
    }

    /// @brief Total P&L = realized + unrealized.
    [[nodiscard]] int64_t total_pnl() const noexcept {
        return realized_pnl() + unrealized_pnl();
    }

    /// @brief Current net position (positive = long, negative = short).
    [[nodiscard]] int64_t position() const noexcept { return position_; }

    /// @brief Total number of fills recorded.
    [[nodiscard]] uint64_t num_trades() const noexcept { return num_trades_; }

    /// @brief Total number of orders sent.
    [[nodiscard]] uint64_t num_orders() const noexcept { return num_orders_; }

    /// @brief Average entry price of the current position.
    [[nodiscard]] int64_t avg_entry_price() const noexcept { return avg_entry_price_; }

    // ─── Risk metrics ────────────────────────────────────────────────────

    /// @brief Maximum drawdown observed (in fixed-point P&L units).
    ///
    /// Drawdown = peak_pnl - current_pnl. This tracks the worst-case
    /// peak-to-trough decline over the life of the tracker.
    [[nodiscard]] int64_t max_drawdown() const noexcept {
        return max_drawdown_;
    }

    /// @brief Record a per-second P&L sample for Sharpe ratio calculation.
    /// @param pnl_sample  The total P&L at this second boundary.
    ///
    /// Call this once per second of simulation time. The Sharpe ratio is
    /// computed over the differences between consecutive samples (returns).
    void record_pnl_sample(int64_t pnl_sample) noexcept {
        if (num_samples_ > 0) {
            // Compute the return (change in P&L from last sample)
            const int64_t ret = pnl_sample - last_sample_;
            returns_[return_write_idx_] = static_cast<double>(ret);
            return_write_idx_ = (return_write_idx_ + 1) % kMaxSamples;
            if (num_returns_ < kMaxSamples) num_returns_++;
        }

        last_sample_ = pnl_sample;
        num_samples_++;
    }

    /// @brief Compute the Sharpe ratio over recorded per-second P&L samples.
    ///
    /// Sharpe = mean(returns) / stdev(returns)
    ///
    /// Returns 0.0 if insufficient data (< 2 samples).
    /// Not annualized — raw per-second Sharpe. Multiply by sqrt(N) to
    /// annualize where N = number of seconds in your target period.
    [[nodiscard]] double sharpe_ratio() const noexcept {
        if (num_returns_ < 2) return 0.0;

        // Compute mean
        double sum = 0.0;
        for (std::size_t i = 0; i < num_returns_; ++i) {
            sum += returns_[i];
        }
        const double mean = sum / static_cast<double>(num_returns_);

        // Compute variance
        double var_sum = 0.0;
        for (std::size_t i = 0; i < num_returns_; ++i) {
            const double diff = returns_[i] - mean;
            var_sum += diff * diff;
        }
        const double variance = var_sum / static_cast<double>(num_returns_ - 1);
        const double stdev = std::sqrt(variance);

        if (stdev < 1e-12) return 0.0;  // Avoid division by zero

        return mean / stdev;
    }

    /// @brief Reset all state.
    void reset() noexcept {
        realized_pnl_    = 0;
        position_        = 0;
        avg_entry_price_ = 0;
        last_price_      = 0;
        num_trades_      = 0;
        num_orders_      = 0;
        peak_pnl_        = 0;
        max_drawdown_    = 0;
        num_samples_     = 0;
        num_returns_     = 0;
        return_write_idx_ = 0;
        last_sample_     = 0;
        returns_.fill(0.0);
    }

private:
    // ─── Drawdown tracking ───────────────────────────────────────────────

    /// @brief Update peak P&L and max drawdown after every fill.
    void update_drawdown() noexcept {
        const int64_t current = total_pnl();
        if (current > peak_pnl_) {
            peak_pnl_ = current;
        }
        const int64_t drawdown = peak_pnl_ - current;
        if (drawdown > max_drawdown_) {
            max_drawdown_ = drawdown;
        }
    }

    // ─── P&L state ───────────────────────────────────────────────────────

    int64_t realized_pnl_    = 0;  ///< Cumulative realized P&L
    int64_t position_        = 0;  ///< Current net position (signed)
    int64_t avg_entry_price_ = 0;  ///< Weighted average entry price
    int64_t last_price_      = 0;  ///< Last mark-to-market price

    // ─── Counters ────────────────────────────────────────────────────────

    uint64_t num_trades_ = 0;      ///< Number of fills
    uint64_t num_orders_ = 0;      ///< Number of orders sent

    // ─── Drawdown state ──────────────────────────────────────────────────

    int64_t peak_pnl_     = 0;     ///< Running peak total P&L
    int64_t max_drawdown_ = 0;     ///< Worst peak-to-trough decline

    // ─── Sharpe ratio state ──────────────────────────────────────────────
    //
    // We store per-second returns (differences between consecutive P&L
    // samples) in a circular buffer. This avoids heap allocation and
    // gives us a rolling Sharpe over the last kMaxSamples seconds.

    static constexpr std::size_t kMaxSamples = 86400;  ///< 24 hours of seconds

    std::array<double, kMaxSamples> returns_ = {};     ///< Circular return buffer
    std::size_t num_samples_     = 0;                  ///< Total samples recorded
    std::size_t num_returns_     = 0;                  ///< Returns in buffer (≤ kMaxSamples)
    std::size_t return_write_idx_ = 0;                 ///< Write position in circular buffer
    int64_t     last_sample_     = 0;                  ///< Previous P&L sample value
};

} // namespace hft
