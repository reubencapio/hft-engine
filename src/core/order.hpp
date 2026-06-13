#pragma once

// =============================================================================
// order.hpp — Core Order and Trade types for the HFT Backtesting Engine
//
// Design decisions:
//   - alignas(64) on Order ensures the struct starts on a cache-line boundary,
//     minimizing false sharing when multiple threads touch adjacent orders.
//   - Fixed-size integer types only (uint64_t, int64_t) — no std::string, no
//     heap allocation. Everything is trivially copyable.
//   - Intrusive doubly-linked list pointers (prev/next) allow O(1) insertion
//     and removal from price-level queues without any separate node allocation.
//   - Prices are stored in integer "ticks" to avoid floating-point comparison
//     issues on the hot path.
//   - Timestamps use raw RDTSC cycle counts (converted to ns externally) for
//     minimal-overhead latency measurement.
//   - static_assert on sizeof(Order) guarantees we never accidentally bloat
//     the struct beyond two cache lines (128 bytes).
// =============================================================================

#include <cstdint>
#include <type_traits>

namespace hft {

// ---------------------------------------------------------------------------
// Side — Bid or Ask
// ---------------------------------------------------------------------------
// Using uint8_t as underlying type to minimize storage. This enum is used in
// hot-path order matching so it must be cheap to compare.
// ---------------------------------------------------------------------------
enum class Side : uint8_t {
    Bid = 0,  // Buy side
    Ask = 1   // Sell side
};

// ---------------------------------------------------------------------------
// OrderType — Classification of order intent
// ---------------------------------------------------------------------------
// Limit:  resting order at a specific price level
// Market: immediate execution at best available price
// Cancel: request to remove a previously submitted limit order
// ---------------------------------------------------------------------------
enum class OrderType : uint8_t {
    Limit  = 0,
    Market = 1,
    Cancel = 2
};

// ---------------------------------------------------------------------------
// Order — Primary order representation
// ---------------------------------------------------------------------------
// Layout rationale (targeting ≤ 128 bytes = 2 cache lines):
//
//   Field            Type          Size   Cumulative
//   ─────────────    ──────────    ────   ──────────
//   order_id         uint64_t       8        8
//   price            int64_t        8       16
//   quantity         int64_t        8       24
//   timestamp_ns     uint64_t       8       32
//   prev             Order*         8       40
//   next             Order*         8       48
//   side             Side(u8)       1       49
//   order_type       OrderType(u8)  1       50
//   (padding)                      14       64
//
// Total: 64 bytes (1 cache line). Well within the 128-byte budget.
//
// The intrusive prev/next pointers form a doubly-linked list used by the
// order book's price-level queues (Phase 2). Orders are allocated from a
// pre-allocated object pool, so these pointers never trigger heap allocation
// on the hot path.
// ---------------------------------------------------------------------------
struct alignas(64) Order {
    // --- Primary fields (hot — accessed during matching) ---
    uint64_t  order_id     = 0;       // Unique order identifier
    int64_t   price        = 0;       // Price in ticks (integer to avoid FP issues)
    int64_t   quantity     = 0;       // Remaining quantity (decremented on partial fills)
    uint64_t  timestamp_ns = 0;       // RDTSC timestamp at order creation

    // --- Intrusive doubly-linked list pointers (for price-level queues) ---
    Order*    prev         = nullptr; // Previous order at same price level
    Order*    next         = nullptr; // Next order at same price level

    // --- Classification fields (still hot, but smaller) ---
    Side      side         = Side::Bid;          // Bid or Ask
    OrderType order_type   = OrderType::Limit;   // Limit, Market, or Cancel
};

// Compile-time guarantees
static_assert(sizeof(Order) <= 128,
    "Order must fit in two cache lines (128 bytes) max");
static_assert(alignof(Order) == 64,
    "Order must be aligned to a cache line (64 bytes)");
static_assert(std::is_trivially_copyable_v<Order>,
    "Order must be trivially copyable for lock-free queues");
static_assert(std::is_trivially_destructible_v<Order>,
    "Order must be trivially destructible for object pool reuse");

// ---------------------------------------------------------------------------
// Trade — Execution record produced by the matching engine
// ---------------------------------------------------------------------------
// Lightweight value type capturing the result of a match between two orders.
// Designed to be pushed into an SPSC queue for downstream consumption
// (strategy module, metrics, logging).
//
// aggressor_side identifies which side was the incoming order (the "taker"):
//   Bid  = an incoming buy order crossed against a resting ask
//   Ask  = an incoming sell order crossed against a resting bid
// ---------------------------------------------------------------------------
struct Trade {
    uint64_t buy_order_id   = 0;         // ID of the buy (bid) order
    uint64_t sell_order_id  = 0;         // ID of the sell (ask) order
    int64_t  price          = 0;         // Execution price in ticks
    int64_t  quantity       = 0;         // Executed quantity
    uint64_t timestamp_ns   = 0;         // RDTSC timestamp at execution
    Side     aggressor_side = Side::Bid; // Which side was the incoming order
};

static_assert(sizeof(Trade) == 48,
    "Trade should be exactly 48 bytes (5 × 8-byte fields + Side + 7-byte padding)");
static_assert(std::is_trivially_copyable_v<Trade>,
    "Trade must be trivially copyable for lock-free queues");

} // namespace hft
