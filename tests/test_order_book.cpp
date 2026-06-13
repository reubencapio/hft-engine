/// @file test_order_book.cpp
/// @brief Google Test suite for the limit order book.

#include <gtest/gtest.h>

#include "core/order_book.hpp"

using hft::OrderBook;
using hft::PriceLevel;

// ─── Empty book ───────────────────────────────────────────────────────────────

TEST(OrderBook, EmptyBook) {
    OrderBook book;
    EXPECT_EQ(book.best_bid(), 0);
    EXPECT_EQ(book.best_ask(), 0);
    EXPECT_EQ(book.best_bid_qty(), 0);
    EXPECT_EQ(book.best_ask_qty(), 0);
    EXPECT_EQ(book.mid_price(), 0);
    EXPECT_EQ(book.spread(), 0);
    EXPECT_EQ(book.num_bids(), 0u);
    EXPECT_EQ(book.num_asks(), 0u);
}

// ─── Add and access levels ────────────────────────────────────────────────────

TEST(OrderBook, AddBidAsk) {
    OrderBook book;
    book.add_bid(1000, 100);
    book.add_ask(1010, 200);

    EXPECT_EQ(book.best_bid(),     1000);
    EXPECT_EQ(book.best_ask(),     1010);
    EXPECT_EQ(book.best_bid_qty(), 100);
    EXPECT_EQ(book.best_ask_qty(), 200);
    EXPECT_EQ(book.spread(),       10);
    EXPECT_EQ(book.mid_price(),    1005);
    EXPECT_EQ(book.num_bids(),     1u);
    EXPECT_EQ(book.num_asks(),     1u);
}

TEST(OrderBook, BidsSortedDescending) {
    OrderBook book;
    book.add_bid(1000, 100);
    book.add_bid(1005, 50);   // better bid
    book.add_bid(995,  200);  // worse bid

    EXPECT_EQ(book.best_bid(), 1005);
    EXPECT_EQ(book.num_bids(), 3u);

    const PriceLevel* bids = book.bids();
    EXPECT_EQ(bids[0].price, 1005);
    EXPECT_EQ(bids[1].price, 1000);
    EXPECT_EQ(bids[2].price,  995);
}

TEST(OrderBook, AsksSortedAscending) {
    OrderBook book;
    book.add_ask(1010, 100);
    book.add_ask(1005, 50);   // better ask
    book.add_ask(1020, 200);  // worse ask

    EXPECT_EQ(book.best_ask(), 1005);
    EXPECT_EQ(book.num_asks(), 3u);

    const PriceLevel* asks = book.asks();
    EXPECT_EQ(asks[0].price, 1005);
    EXPECT_EQ(asks[1].price, 1010);
    EXPECT_EQ(asks[2].price, 1020);
}

// ─── Accumulate quantity at existing level ────────────────────────────────────

TEST(OrderBook, AccumulateAtExistingLevel) {
    OrderBook book;
    book.add_bid(1000, 100);
    book.add_bid(1000, 50);

    EXPECT_EQ(book.best_bid_qty(), 150);
    EXPECT_EQ(book.num_bids(), 1u);
}

// ─── Remove quantity ──────────────────────────────────────────────────────────

TEST(OrderBook, RemovePartialQuantity) {
    OrderBook book;
    book.add_bid(1000, 100);
    book.remove_bid(1000, 40);

    EXPECT_EQ(book.best_bid_qty(), 60);
    EXPECT_EQ(book.num_bids(), 1u);
}

TEST(OrderBook, RemoveFullQuantityDeletesLevel) {
    OrderBook book;
    book.add_bid(1000, 100);
    book.add_bid(990, 200);
    book.remove_bid(1000, 100);

    EXPECT_EQ(book.best_bid(), 990);
    EXPECT_EQ(book.num_bids(), 1u);
}

TEST(OrderBook, RemoveAskLevel) {
    OrderBook book;
    book.add_ask(1010, 100);
    book.add_ask(1020, 200);
    book.remove_ask(1010, 100);

    EXPECT_EQ(book.best_ask(), 1020);
    EXPECT_EQ(book.num_asks(), 1u);
}

// ─── Clear ────────────────────────────────────────────────────────────────────

TEST(OrderBook, Clear) {
    OrderBook book;
    book.add_bid(1000, 100);
    book.add_ask(1010, 100);
    book.clear();

    EXPECT_EQ(book.num_bids(), 0u);
    EXPECT_EQ(book.num_asks(), 0u);
    EXPECT_EQ(book.best_bid(), 0);
    EXPECT_EQ(book.best_ask(), 0);
}

// ─── Symbol ───────────────────────────────────────────────────────────────────

TEST(OrderBook, SymbolAccessor) {
    OrderBook book("AAPL");
    EXPECT_STREQ(book.symbol(), "AAPL");
}

// ─── Mid-price with one side empty ───────────────────────────────────────────

TEST(OrderBook, MidPriceOneSideEmpty) {
    OrderBook book;
    book.add_bid(1000, 100);
    EXPECT_EQ(book.mid_price(), 0);  // no asks, so mid is undefined → 0

    book.clear();
    book.add_ask(1010, 100);
    EXPECT_EQ(book.mid_price(), 0);  // no bids
}
