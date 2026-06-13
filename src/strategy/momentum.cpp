/// @file momentum.cpp
/// @brief Momentum / trend-following strategy implementation.
///
/// Signal computation:
///   change_bps = (current_mid - lookback_mid) * 10000 / lookback_mid
///   change_bps > threshold  → buy signal
///   change_bps < -threshold → sell signal

#include "momentum.hpp"

#include <cstdlib>      // std::abs
#include <ctime>        // clock_gettime, CLOCK_MONOTONIC
#include <x86intrin.h>  // __rdtscp

namespace hft {

MomentumStrategy::MomentumStrategy(uint64_t lookback_ns,
                                   int64_t threshold_bps,
                                   int64_t order_qty) noexcept
    : lookback_ns_(lookback_ns)
    , threshold_bps_(threshold_bps)
    , order_qty_(order_qty)
    , ticks_per_ns_(calibrate_ticks_per_ns())
    , lookback_ticks_(static_cast<uint64_t>(
          static_cast<double>(lookback_ns) * ticks_per_ns_))
{}

uint64_t MomentumStrategy::rdtsc() noexcept {
    unsigned int aux;
    return __rdtscp(&aux);
}

double MomentumStrategy::calibrate_ticks_per_ns() noexcept {
    struct timespec ts_start{}, ts_end{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts_start);
    const uint64_t tsc_start = rdtsc();

    const uint64_t start_ns =
        static_cast<uint64_t>(ts_start.tv_sec) * 1'000'000'000ULL +
        static_cast<uint64_t>(ts_start.tv_nsec);

    uint64_t now_ns = start_ns;
    while (now_ns - start_ns < 1'000'000ULL) {
        ::clock_gettime(CLOCK_MONOTONIC, &ts_end);
        now_ns = static_cast<uint64_t>(ts_end.tv_sec) * 1'000'000'000ULL +
                 static_cast<uint64_t>(ts_end.tv_nsec);
    }

    const uint64_t tsc_end   = rdtsc();
    const uint64_t delta_tsc = tsc_end - tsc_start;
    const uint64_t delta_ns  = now_ns - start_ns;

    if (delta_ns == 0) return 3.0;
    return static_cast<double>(delta_tsc) / static_cast<double>(delta_ns);
}

void MomentumStrategy::on_trade(const Trade& trade) {
    if (pnl && open_order_id_ != 0) {
        const bool buy_fill  = (trade.buy_order_id  == open_order_id_);
        const bool sell_fill = (trade.sell_order_id == open_order_id_);

        if (buy_fill || sell_fill) {
            pnl->on_fill(buy_fill, trade.quantity, trade.price);
            pnl->mark_to_market(trade.price);
            open_order_id_ = 0;
        }
    }

    if (pnl && pnl->position() == 0) {
        has_position_ = false;
    }
}

void MomentumStrategy::on_book_update(const OrderBook& book) {
    const int64_t mid = book.mid_price();
    if (mid <= 0) return;

    if (pnl) pnl->mark_to_market(mid);

    const uint64_t now_tsc = rdtsc();
    samples_[write_idx_] = PriceSample{now_tsc, mid};
    write_idx_ = (write_idx_ + 1) % kMaxSamples;
    if (num_samples_ < kMaxSamples) num_samples_++;

    if (has_position_) return;

    const PriceSample* lookback = find_lookback_sample(now_tsc);
    if (!lookback) return;

    const int64_t lookback_mid = lookback->mid_price;
    if (lookback_mid == 0) return;

    const int64_t delta      = mid - lookback_mid;
    const int64_t change_bps = (delta * 10000) / lookback_mid;

    if (change_bps > threshold_bps_) {
        send_market_order(Side::Bid, order_qty_);
        has_position_ = true;
    } else if (change_bps < -threshold_bps_) {
        send_market_order(Side::Ask, order_qty_);
        has_position_ = true;
    }
}

void MomentumStrategy::on_timer(uint64_t /*timestamp_ns*/) {}

std::string MomentumStrategy::name() const {
    return "Momentum";
}

const PriceSample* MomentumStrategy::find_lookback_sample(
    uint64_t current_tick) const noexcept
{
    if (num_samples_ < 2) return nullptr;

    const uint64_t oldest_allowed = (current_tick > lookback_ticks_)
                                    ? (current_tick - lookback_ticks_)
                                    : 0;

    const PriceSample* best  = nullptr;
    const std::size_t  count = num_samples_;

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t idx = (write_idx_ + kMaxSamples - 1 - i) % kMaxSamples;
        const PriceSample& sample = samples_[idx];

        if (sample.rdtsc_tick < oldest_allowed) break;
        best = &sample;
    }

    return best;
}

void MomentumStrategy::send_market_order(Side side, int64_t qty) noexcept {
    if (!order_queue) return;

    Order order{};
    order.order_id     = next_order_id_++;
    order.order_type   = OrderType::Market;
    order.side         = side;
    order.price        = 0;
    order.quantity     = qty;
    order.timestamp_ns = 0;

    if (order_queue->push(order)) {
        if (pnl) pnl->on_order_sent();
        open_order_id_ = order.order_id;
    }
}

} // namespace hft
