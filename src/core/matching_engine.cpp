/// @file matching_engine.cpp
/// @brief Matching engine implementation.

#include "matching_engine.hpp"

#include <algorithm>
#include <immintrin.h>  // _mm_pause()
#include <pthread.h>    // pthread_setaffinity_np
#include <sched.h>      // cpu_set_t

namespace hft {

// ─── Construction ─────────────────────────────────────────────────────────────

MatchingEngine::MatchingEngine(SPSCQueue<Order, 65536>* input,
                               SPSCQueue<Trade, 65536>* output) noexcept
    : input_queue_(input), output_queue_(output) {}

MatchingEngine::~MatchingEngine() {
    stop();
}

// ─── Real-time mode ───────────────────────────────────────────────────────────

void MatchingEngine::start(int cpu_core) {
    running_.store(true, std::memory_order_release);
    engine_thread_ = std::thread([this, cpu_core]() {
        if (cpu_core >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_core, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        }
        run_loop();
    });
}

void MatchingEngine::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (engine_thread_.joinable()) {
        engine_thread_.join();
    }
}

void MatchingEngine::run_loop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        Order order;
        if (!input_queue_->pop(order)) {
            _mm_pause();
            continue;
        }

        const uint64_t t0 = rdtsc();
        auto trades = do_match(order);
        latency_hist_.record(t0, rdtsc());

        for (const auto& trade : trades) {
            // Spin-wait if output queue is full — consumer should drain quickly.
            while (!output_queue_->push(trade)) {
                _mm_pause();
            }
        }
    }
}

// ─── Backtest mode ────────────────────────────────────────────────────────────

std::vector<Trade> MatchingEngine::process_order(Order order) noexcept {
    const uint64_t t0 = rdtsc();
    auto trades = do_match(order);
    latency_hist_.record(t0, rdtsc());
    return trades;
}

void MatchingEngine::reset() noexcept {
    book_.clear();
    latency_hist_.reset();
}

// ─── Core matching logic ──────────────────────────────────────────────────────

std::vector<Trade> MatchingEngine::do_match(Order& order) noexcept {
    std::vector<Trade> trades;

    // ── Cancel ──────────────────────────────────────────────────────────────
    if (order.order_type == OrderType::Cancel) {
        if (order.side == Side::Bid) {
            book_.remove_bid(order.price, order.quantity);
        } else {
            book_.remove_ask(order.price, order.quantity);
        }
        return trades;
    }

    const bool is_buy = (order.side == Side::Bid);

    // ── Match loop ───────────────────────────────────────────────────────────
    while (order.quantity > 0) {
        if (is_buy) {
            const int64_t best_ask = book_.best_ask();
            if (best_ask == 0) break;

            const int64_t ask_qty = book_.best_ask_qty();
            if (ask_qty <= 0) break;

            const bool can_match = (order.order_type == OrderType::Market) ||
                                   (order.price >= best_ask);
            if (!can_match) break;

            const int64_t  fill_qty         = std::min(order.quantity, ask_qty);
            const uint64_t passive_order_id = book_.best_ask_order_id();

            Trade t;
            t.buy_order_id   = order.order_id;
            t.sell_order_id  = passive_order_id;
            t.price          = best_ask;
            t.quantity       = fill_qty;
            t.timestamp_ns   = order.timestamp_ns;
            t.aggressor_side = Side::Bid;

            book_.remove_ask(best_ask, fill_qty);
            order.quantity -= fill_qty;
            trades.push_back(t);
        } else {
            const int64_t best_bid = book_.best_bid();
            if (best_bid == 0) break;

            const int64_t bid_qty = book_.best_bid_qty();
            if (bid_qty <= 0) break;

            const bool can_match = (order.order_type == OrderType::Market) ||
                                   (order.price <= best_bid);
            if (!can_match) break;

            const int64_t  fill_qty         = std::min(order.quantity, bid_qty);
            const uint64_t passive_order_id = book_.best_bid_order_id();

            Trade t;
            t.buy_order_id   = passive_order_id;
            t.sell_order_id  = order.order_id;
            t.price          = best_bid;
            t.quantity       = fill_qty;
            t.timestamp_ns   = order.timestamp_ns;
            t.aggressor_side = Side::Ask;

            book_.remove_bid(best_bid, fill_qty);
            order.quantity -= fill_qty;
            trades.push_back(t);
        }
    }

    // ── Post remainder for limit orders ─────────────────────────────────────
    if (order.quantity > 0 && order.order_type == OrderType::Limit) {
        if (is_buy) {
            book_.add_bid(order.price, order.quantity, order.order_id);
        } else {
            book_.add_ask(order.price, order.quantity, order.order_id);
        }
    }

    return trades;
}

} // namespace hft
