/// @file test_matching_engine.cpp
/// @brief Google Test suite for the matching engine.

#include <gtest/gtest.h>

#include "core/matching_engine.hpp"

using hft::MatchingEngine;
using hft::Order;
using hft::OrderType;
using hft::Side;
using hft::Trade;

namespace {

// Helper: build a limit order
Order make_limit(uint64_t id, Side side, int64_t price, int64_t qty,
                 uint64_t ns = 0) {
    Order o{};
    o.order_id     = id;
    o.order_type   = OrderType::Limit;
    o.side         = side;
    o.price        = price;
    o.quantity     = qty;
    o.timestamp_ns = ns;
    return o;
}

// Helper: build a market order
Order make_market(uint64_t id, Side side, int64_t qty) {
    Order o{};
    o.order_id   = id;
    o.order_type = OrderType::Market;
    o.side       = side;
    o.price      = 0;
    o.quantity   = qty;
    return o;
}

// Helper: build a cancel order
Order make_cancel(uint64_t id, Side side, int64_t price, int64_t qty) {
    Order o{};
    o.order_id   = id;
    o.order_type = OrderType::Cancel;
    o.side       = side;
    o.price      = price;
    o.quantity   = qty;
    return o;
}

} // anonymous namespace

// ─── No fill: price miss ──────────────────────────────────────────────────────

TEST(MatchingEngine, NoFillPriceMiss) {
    MatchingEngine engine;

    // Post ask at 1010
    auto t1 = engine.process_order(make_limit(1, Side::Ask, 1010, 100));
    EXPECT_TRUE(t1.empty());
    EXPECT_EQ(engine.book().best_ask(), 1010);

    // Bid at 1005 — below ask, should not match
    auto t2 = engine.process_order(make_limit(2, Side::Bid, 1005, 100));
    EXPECT_TRUE(t2.empty());
    EXPECT_EQ(engine.book().best_bid(), 1005);
}

// ─── Full fill ────────────────────────────────────────────────────────────────

TEST(MatchingEngine, FullFill) {
    MatchingEngine engine;

    // Post resting ask at 1000
    engine.process_order(make_limit(1, Side::Ask, 1000, 100));

    // Aggressive bid at 1000 — full match
    auto trades = engine.process_order(make_limit(2, Side::Bid, 1000, 100));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price,        1000);
    EXPECT_EQ(trades[0].quantity,     100);
    EXPECT_EQ(trades[0].buy_order_id, 2u);  // aggressor is the bid
    EXPECT_EQ(trades[0].aggressor_side, Side::Bid);

    // Ask level should be fully consumed
    EXPECT_EQ(engine.book().best_ask(), 0);
    EXPECT_EQ(engine.book().num_asks(), 0u);
    // Bid also fully consumed (nothing left to post)
    EXPECT_EQ(engine.book().best_bid(), 0);
}

// ─── Partial fill ─────────────────────────────────────────────────────────────

TEST(MatchingEngine, PartialFill) {
    MatchingEngine engine;

    // Resting ask: 100 shares at 1000
    engine.process_order(make_limit(1, Side::Ask, 1000, 100));

    // Aggressive bid: 60 shares — partial match
    auto trades = engine.process_order(make_limit(2, Side::Bid, 1000, 60));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 60);

    // 40 shares should remain at ask
    EXPECT_EQ(engine.book().best_ask(),     1000);
    EXPECT_EQ(engine.book().best_ask_qty(), 40);
}

// ─── Partial fill: aggressive larger than resting ────────────────────────────

TEST(MatchingEngine, PartialFillAggressorLarger) {
    MatchingEngine engine;

    // Resting ask: 50 shares at 1000
    engine.process_order(make_limit(1, Side::Ask, 1000, 50));

    // Aggressive bid: 100 shares — fills 50, posts remainder 50 as bid
    auto trades = engine.process_order(make_limit(2, Side::Bid, 1000, 100));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 50);

    // Ask fully consumed
    EXPECT_EQ(engine.book().num_asks(), 0u);
    // Remainder posted as bid
    EXPECT_EQ(engine.book().best_bid(),     1000);
    EXPECT_EQ(engine.book().best_bid_qty(), 50);
}

// ─── Multi-level fill ─────────────────────────────────────────────────────────

TEST(MatchingEngine, MultiLevelFill) {
    MatchingEngine engine;

    engine.process_order(make_limit(1, Side::Ask, 1000, 30));
    engine.process_order(make_limit(2, Side::Ask, 1001, 40));
    engine.process_order(make_limit(3, Side::Ask, 1002, 50));

    // Market buy for 100 — should consume from 1000, then 1001, then 1002
    auto trades = engine.process_order(make_market(4, Side::Bid, 100));

    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].price,    1000);
    EXPECT_EQ(trades[0].quantity,   30);
    EXPECT_EQ(trades[1].price,    1001);
    EXPECT_EQ(trades[1].quantity,   40);
    EXPECT_EQ(trades[2].price,    1002);
    EXPECT_EQ(trades[2].quantity,   30);  // only 30 needed to reach 100 total

    EXPECT_EQ(engine.book().best_ask_qty(), 20);  // 50 - 30 remain at 1002
}

// ─── Market order: empty book ────────────────────────────────────────────────

TEST(MatchingEngine, MarketOrderEmptyBook) {
    MatchingEngine engine;
    auto trades = engine.process_order(make_market(1, Side::Bid, 100));
    EXPECT_TRUE(trades.empty());
}

// ─── Cancel ───────────────────────────────────────────────────────────────────

TEST(MatchingEngine, CancelReducesQuantity) {
    MatchingEngine engine;

    engine.process_order(make_limit(1, Side::Bid, 1000, 100));
    EXPECT_EQ(engine.book().best_bid_qty(), 100);

    engine.process_order(make_cancel(1, Side::Bid, 1000, 50));
    EXPECT_EQ(engine.book().best_bid_qty(), 50);
}

TEST(MatchingEngine, CancelFullQuantityRemovesLevel) {
    MatchingEngine engine;

    engine.process_order(make_limit(1, Side::Bid, 1000, 100));
    engine.process_order(make_cancel(1, Side::Bid, 1000, 100));

    EXPECT_EQ(engine.book().best_bid(), 0);
    EXPECT_EQ(engine.book().num_bids(), 0u);
}

// ─── Reset ────────────────────────────────────────────────────────────────────

TEST(MatchingEngine, Reset) {
    MatchingEngine engine;

    engine.process_order(make_limit(1, Side::Bid, 1000, 100));
    engine.process_order(make_limit(2, Side::Ask, 1010, 200));

    engine.reset();

    EXPECT_EQ(engine.book().num_bids(), 0u);
    EXPECT_EQ(engine.book().num_asks(), 0u);
    EXPECT_EQ(engine.latency_histogram().count(), 0u);
}

// ─── Latency histogram records samples ───────────────────────────────────────

TEST(MatchingEngine, LatencyHistogramUpdated) {
    MatchingEngine engine;

    engine.process_order(make_limit(1, Side::Ask, 1000, 100));
    engine.process_order(make_limit(2, Side::Bid, 1000, 100));

    // Two process_order calls → at least 2 histogram entries
    EXPECT_GE(engine.latency_histogram().count(), 2u);
}
