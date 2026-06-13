#pragma once

/// @file order_book.hpp
/// @brief Limit order book with price-level aggregation.
///
/// This is a simplified but performant order book suitable for backtesting.
/// It maintains sorted bid and ask levels in fixed-capacity arrays to avoid
/// heap allocation on the hot path.
///
/// Price representation: all prices are int64_t in fixed-point format
/// (e.g. dollars × 10000 so $150.25 = 1502500). This avoids floating-point
/// non-determinism in the matching engine.
///
/// Level storage: uses flat sorted arrays rather than std::map to keep
/// data cache-resident. Typical order books rarely exceed a few hundred
/// levels, so linear scans are faster than tree lookups due to prefetching.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace hft {

/// @brief A single price level in the order book.
struct PriceLevel {
    int64_t  price        = 0;  ///< Price (fixed-point)
    int64_t  quantity     = 0;  ///< Total quantity at this level
    uint32_t count        = 0;  ///< Number of orders at this level
    uint64_t last_order_id = 0; ///< Order ID of the last resting order added here
};

/// @brief Limit order book with fixed-capacity level arrays.
///
/// Bids are sorted descending (best bid = index 0).
/// Asks are sorted ascending  (best ask = index 0).
///
/// Maximum depth is compile-time configurable via kMaxLevels.
class OrderBook {
public:
    static constexpr std::size_t kMaxLevels = 256;

    OrderBook() noexcept {
        std::memset(symbol_, 0, sizeof(symbol_));
    }

    explicit OrderBook(const char* sym) noexcept {
        std::memset(symbol_, 0, sizeof(symbol_));
        if (sym) {
            std::size_t len = std::strlen(sym);
            if (len > sizeof(symbol_)) len = sizeof(symbol_);
            std::memcpy(symbol_, sym, len);
        }
    }

    // ─── Accessors ───────────────────────────────────────────────────────

    /// @brief Best bid price, or 0 if book is empty on bid side.
    [[nodiscard]] int64_t best_bid() const noexcept {
        return (num_bids_ > 0) ? bids_[0].price : 0;
    }

    /// @brief Best ask price, or 0 if book is empty on ask side.
    [[nodiscard]] int64_t best_ask() const noexcept {
        return (num_asks_ > 0) ? asks_[0].price : 0;
    }

    /// @brief Best bid quantity.
    [[nodiscard]] int64_t best_bid_qty() const noexcept {
        return (num_bids_ > 0) ? bids_[0].quantity : 0;
    }

    /// @brief Best ask quantity.
    [[nodiscard]] int64_t best_ask_qty() const noexcept {
        return (num_asks_ > 0) ? asks_[0].quantity : 0;
    }

    /// @brief Order ID of the last order resting at the best bid.
    [[nodiscard]] uint64_t best_bid_order_id() const noexcept {
        return (num_bids_ > 0) ? bids_[0].last_order_id : 0;
    }

    /// @brief Order ID of the last order resting at the best ask.
    [[nodiscard]] uint64_t best_ask_order_id() const noexcept {
        return (num_asks_ > 0) ? asks_[0].last_order_id : 0;
    }

    /// @brief Mid-price = (best_bid + best_ask) / 2.
    /// Returns 0 if either side is empty.
    [[nodiscard]] int64_t mid_price() const noexcept {
        if (num_bids_ == 0 || num_asks_ == 0) return 0;
        return (bids_[0].price + asks_[0].price) / 2;
    }

    /// @brief Spread in price units.
    [[nodiscard]] int64_t spread() const noexcept {
        if (num_bids_ == 0 || num_asks_ == 0) return 0;
        return asks_[0].price - bids_[0].price;
    }

    /// @brief Number of bid levels.
    [[nodiscard]] std::size_t num_bids() const noexcept { return num_bids_; }

    /// @brief Number of ask levels.
    [[nodiscard]] std::size_t num_asks() const noexcept { return num_asks_; }

    /// @brief Access bid levels (sorted descending by price).
    [[nodiscard]] const PriceLevel* bids() const noexcept { return bids_.data(); }

    /// @brief Access ask levels (sorted ascending by price).
    [[nodiscard]] const PriceLevel* asks() const noexcept { return asks_.data(); }

    /// @brief Symbol accessor.
    [[nodiscard]] const char* symbol() const noexcept { return symbol_; }

    // ─── Mutators ────────────────────────────────────────────────────────

    /// @brief Add quantity to a bid level. Inserts the level if new.
    void add_bid(int64_t price, int64_t qty, uint64_t order_id = 0) noexcept {
        add_level(bids_, num_bids_, price, qty, order_id, /*descending=*/true);
    }

    /// @brief Add quantity to an ask level. Inserts the level if new.
    void add_ask(int64_t price, int64_t qty, uint64_t order_id = 0) noexcept {
        add_level(asks_, num_asks_, price, qty, order_id, /*descending=*/false);
    }

    /// @brief Remove quantity from a bid level. Removes level if qty hits 0.
    void remove_bid(int64_t price, int64_t qty) noexcept {
        remove_level(bids_, num_bids_, price, qty);
    }

    /// @brief Remove quantity from an ask level. Removes level if qty hits 0.
    void remove_ask(int64_t price, int64_t qty) noexcept {
        remove_level(asks_, num_asks_, price, qty);
    }

    /// @brief Clear all levels.
    void clear() noexcept {
        num_bids_ = 0;
        num_asks_ = 0;
    }

private:
    // ─── Internal helpers ────────────────────────────────────────────────

    /// @brief Add quantity at a price level, inserting if the level doesn't exist.
    void add_level(std::array<PriceLevel, kMaxLevels>& levels,
                   std::size_t& count,
                   int64_t price, int64_t qty, uint64_t order_id,
                   bool descending) noexcept
    {
        // Search for existing level
        for (std::size_t i = 0; i < count; ++i) {
            if (levels[i].price == price) {
                levels[i].quantity += qty;
                levels[i].count++;
                if (order_id != 0) levels[i].last_order_id = order_id;
                return;
            }
        }

        if (count >= kMaxLevels) return;

        std::size_t pos = count;
        for (std::size_t i = 0; i < count; ++i) {
            bool should_insert = descending
                ? (price > levels[i].price)
                : (price < levels[i].price);
            if (should_insert) {
                pos = i;
                break;
            }
        }

        for (std::size_t i = count; i > pos; --i) {
            levels[i] = levels[i - 1];
        }

        levels[pos] = PriceLevel{price, qty, 1, order_id};
        count++;
    }

    /// @brief Remove quantity from a price level. Removes the level entirely
    ///        if quantity drops to zero or below.
    void remove_level(std::array<PriceLevel, kMaxLevels>& levels,
                      std::size_t& count,
                      int64_t price, int64_t qty) noexcept
    {
        for (std::size_t i = 0; i < count; ++i) {
            if (levels[i].price == price) {
                levels[i].quantity -= qty;
                if (levels[i].quantity <= 0) {
                    // Shift left to remove
                    for (std::size_t j = i; j + 1 < count; ++j) {
                        levels[j] = levels[j + 1];
                    }
                    count--;
                }
                return;
            }
        }
    }

    // ─── Data ────────────────────────────────────────────────────────────

    char symbol_[8] = {};

    std::array<PriceLevel, kMaxLevels> bids_ = {};
    std::size_t num_bids_ = 0;

    std::array<PriceLevel, kMaxLevels> asks_ = {};
    std::size_t num_asks_ = 0;
};

} // namespace hft
