#pragma once

/// @file itch_parser.hpp
/// @brief NASDAQ ITCH 5.0 binary protocol parser using mmap.
///
/// ITCH 5.0 is a direct data feed protocol used by NASDAQ. Messages are
/// variable-length, big-endian (network byte order), and arrive as a
/// continuous binary stream.
///
/// This parser mmap's the entire file and walks through it linearly,
/// converting relevant message types into our Order/Trade structs:
///
///   'A' — Add Order (no MPID attribution)   → 36 bytes
///   'D' — Order Delete                       → 19 bytes
///   'E' — Order Executed                     → 31 bytes
///   'P' — Trade (non-cross)                  → 44 bytes
///
/// Other message types (system events, stock directory, etc.) are skipped.
///
/// ITCH messages are prefixed by a 2-byte big-endian length field that
/// indicates the payload size (not including the 2-byte prefix itself).
///
/// Endianness: All multi-byte fields in ITCH are big-endian. We use
/// __builtin_bswap{16,32,64} for conversion (GCC/Clang intrinsics).
///
/// @see https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf

#include "../core/order.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hft {

/// @brief Parser for NASDAQ ITCH 5.0 binary feed data.
///
/// Usage:
///   ITCHParser parser("/path/to/feed.itch");
///   std::vector<Order> orders = parser.parse_all();
///
/// The file is mmap'd on construction and munmap'd on destruction.
/// No copies of the file data are made during parsing — we read
/// directly from the mapped memory region.
class ITCHParser {
public:
    /// @brief Construct parser and mmap the given file.
    /// @param filename  Path to the ITCH 5.0 binary data file.
    /// @throws std::runtime_error if the file cannot be opened or mmap'd.
    explicit ITCHParser(const std::string& filename);

    /// @brief Destructor — unmaps the file.
    ~ITCHParser();

    // Non-copyable (owns mmap'd resource)
    ITCHParser(const ITCHParser&) = delete;
    ITCHParser& operator=(const ITCHParser&) = delete;

    // Movable
    ITCHParser(ITCHParser&& other) noexcept;
    ITCHParser& operator=(ITCHParser&& other) noexcept;

    /// @brief Parse the entire file and return all orders/trades as Order structs.
    ///
    /// This walks the file linearly. Each supported message type is
    /// converted to an Order:
    ///   - 'A' → OrderType::Limit   (add order)
    ///   - 'D' → OrderType::Cancel  (delete order)
    ///   - 'E' → OrderType::Market  (execution, treated as fill)
    ///   - 'P' → OrderType::Market  (trade message)
    ///
    /// Unsupported message types are silently skipped.
    ///
    /// @return Vector of parsed Orders in file order.
    [[nodiscard]] std::vector<Order> parse_all() const;

    /// @brief Number of bytes in the mapped file.
    [[nodiscard]] std::size_t file_size() const noexcept { return size_; }

private:
    // ─── Endianness helpers ──────────────────────────────────────────────
    // ITCH is big-endian; x86 is little-endian. These convert in-place.

    /// @brief Read a big-endian uint16_t from a raw pointer.
    static uint16_t read_be16(const uint8_t* p) noexcept;

    /// @brief Read a big-endian uint32_t from a raw pointer.
    static uint32_t read_be32(const uint8_t* p) noexcept;

    /// @brief Read a big-endian uint64_t from a raw pointer.
    static uint64_t read_be64(const uint8_t* p) noexcept;

    /// @brief Read a 6-byte big-endian timestamp (ITCH uses 6-byte nanosecond
    ///        timestamps since midnight).
    static uint64_t read_be48(const uint8_t* p) noexcept;

    // ─── Per-message-type parsers ────────────────────────────────────────

    /// @brief Parse an 'A' (Add Order) message.
    /// @param p  Pointer to the start of the message body (after length prefix).
    /// @param[out] order  Populated on return.
    static void parse_add_order(const uint8_t* p, Order& order) noexcept;

    /// @brief Parse a 'D' (Delete Order) message.
    static void parse_delete_order(const uint8_t* p, Order& order) noexcept;

    /// @brief Parse an 'E' (Order Executed) message.
    static void parse_executed(const uint8_t* p, Order& order) noexcept;

    /// @brief Parse a 'P' (Trade) message.
    static void parse_trade(const uint8_t* p, Order& order) noexcept;

    // ─── Data ────────────────────────────────────────────────────────────

    uint8_t*    data_ = nullptr;   ///< Pointer to mmap'd region
    std::size_t size_ = 0;         ///< Size of mmap'd region in bytes
};

} // namespace hft
