/// @file market_maker.cpp
/// @brief Market-making strategy implementation.
///
/// Order placement logic:
///   bid_price = best_bid - (spread_ticks / 2)
///   ask_price = best_ask + (spread_ticks / 2)
///
///   If spread_ticks = 2 and BBO is 1000/1002:
///     bid = 999, ask = 1003 (4-tick wide quoted spread)
///
/// Position risk: when abs(position) > max_position, a market order flattens.

#include "market_maker.hpp"

#include <cstdlib>  // std::abs

namespace hft {

MarketMaker::MarketMaker(int64_t spread_ticks,
                         int64_t order_qty,
                         int64_t max_position) noexcept
    : spread_ticks_(spread_ticks)
    , order_qty_(order_qty)
    , max_position_(max_position)
{}

void MarketMaker::on_trade(const Trade& trade) {
    if (pnl) {
        // Match against tracked order IDs to determine our fill direction.
        const bool bid_filled = (bid_order_id_ != 0 &&
                                 trade.buy_order_id == bid_order_id_);
        const bool ask_filled = (ask_order_id_ != 0 &&
                                 trade.sell_order_id == ask_order_id_);

        if (bid_filled) {
            pnl->on_fill(true, trade.quantity, trade.price);
            bid_order_id_    = 0;
            has_open_orders_ = false;
        }
        if (ask_filled) {
            pnl->on_fill(false, trade.quantity, trade.price);
            ask_order_id_    = 0;
            has_open_orders_ = false;
        }
        pnl->mark_to_market(trade.price);
    }

    if (pnl && std::abs(pnl->position()) > max_position_) {
        flatten_position();
    }
}

void MarketMaker::on_book_update(const OrderBook& book) {
    if (has_open_orders_) return;

    const int64_t best_bid = book.best_bid();
    const int64_t best_ask = book.best_ask();
    if (best_bid <= 0 || best_ask <= 0) return;

    if (pnl) pnl->mark_to_market(book.mid_price());

    const int64_t half_spread = spread_ticks_ / 2;
    const int64_t bid_price   = best_bid - half_spread;
    const int64_t ask_price   = best_ask + half_spread;

    if (bid_price <= 0 || ask_price <= 0 || bid_price >= ask_price) return;

    send_limit_order(Side::Bid, bid_price, order_qty_);
    send_limit_order(Side::Ask, ask_price, order_qty_);

    has_open_orders_ = true;
}

void MarketMaker::on_timer(uint64_t /*timestamp_ns*/) {}

std::string MarketMaker::name() const {
    return "MarketMaker";
}

void MarketMaker::send_limit_order(Side side, int64_t price, int64_t qty) noexcept {
    if (!order_queue) return;

    Order order{};
    order.order_id     = next_order_id_++;
    order.order_type   = OrderType::Limit;
    order.side         = side;
    order.price        = price;
    order.quantity     = qty;
    order.timestamp_ns = 0;

    if (order_queue->push(order)) {
        if (pnl) pnl->on_order_sent();
        if (side == Side::Bid) bid_order_id_ = order.order_id;
        else                   ask_order_id_ = order.order_id;
    }
}

void MarketMaker::flatten_position() noexcept {
    if (!order_queue || !pnl) return;

    const int64_t pos = pnl->position();
    if (pos == 0) return;

    Order order{};
    order.order_id     = next_order_id_++;
    order.order_type   = OrderType::Market;
    order.side         = (pos > 0) ? Side::Ask : Side::Bid;
    order.price        = 0;
    order.quantity     = std::abs(pos);
    order.timestamp_ns = 0;

    if (order_queue->push(order)) {
        if (pnl) pnl->on_order_sent();
    }
}

} // namespace hft
