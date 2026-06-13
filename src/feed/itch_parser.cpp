/// @file itch_parser.cpp
/// @brief NASDAQ ITCH 5.0 binary protocol parser implementation.
///
/// ITCH 5.0 message format reference:
///   - Every message is preceded by a 2-byte big-endian length field.
///   - The length field gives the size of the payload (excluding the 2
///     length bytes themselves).
///   - The first byte of the payload is the message type character.
///
/// Supported message types and their payload sizes:
///   'A' Add Order (no MPID):  36 bytes
///       Offset  Field                Size
///       0       Message Type         1
///       1       Stock Locate         2
///       3       Tracking Number      2
///       5       Timestamp            6  (nanoseconds since midnight)
///       11      Order Reference      8
///       19      Buy/Sell Indicator    1  ('B' or 'S')
///       20      Shares               4
///       24      Stock                8  (space-padded ASCII)
///       32      Price                4  (fixed-point, 4 decimal places)
///
///   'D' Order Delete:  19 bytes
///       0       Message Type         1
///       1       Stock Locate         2
///       3       Tracking Number      2
///       5       Timestamp            6
///       11      Order Reference      8
///
///   'E' Order Executed:  31 bytes
///       0       Message Type         1
///       1       Stock Locate         2
///       3       Tracking Number      2
///       5       Timestamp            6
///       11      Order Reference      8
///       19      Executed Shares      4
///       23      Match Number         8
///
///   'P' Trade (non-cross):  44 bytes
///       0       Message Type         1
///       1       Stock Locate         2
///       3       Tracking Number      2
///       5       Timestamp            6
///       11      Order Reference      8
///       19      Buy/Sell Indicator    1
///       20      Shares               4
///       24      Stock                8
///       32      Price                4
///       36      Match Number         8

#include "itch_parser.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>      // open()
#include <stdexcept>
#include <sys/mman.h>   // mmap(), munmap()
#include <sys/stat.h>   // fstat()
#include <unistd.h>     // close()

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

ITCHParser::ITCHParser(const std::string& filename) {
    // Open file read-only
    int fd = ::open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("ITCHParser: cannot open file '" + filename +
                                 "': " + std::strerror(errno));
    }

    // Get file size via fstat
    struct stat sb{};
    if (::fstat(fd, &sb) < 0) {
        ::close(fd);
        throw std::runtime_error("ITCHParser: fstat failed on '" + filename +
                                 "': " + std::strerror(errno));
    }
    size_ = static_cast<std::size_t>(sb.st_size);

    if (size_ == 0) {
        ::close(fd);
        throw std::runtime_error("ITCHParser: file '" + filename + "' is empty");
    }

    // mmap the entire file. MAP_PRIVATE so the kernel can do read-ahead and
    // we never accidentally write back. MAP_POPULATE to prefault pages and
    // avoid minor page faults during parsing.
    data_ = static_cast<uint8_t*>(
        ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));

    // Close the fd immediately — mmap keeps its own reference to the inode.
    ::close(fd);

    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        throw std::runtime_error("ITCHParser: mmap failed on '" + filename +
                                 "': " + std::strerror(errno));
    }

    // Advise the kernel that we'll read sequentially. This enables aggressive
    // read-ahead, which is ideal for our linear scan.
    ::madvise(data_, size_, MADV_SEQUENTIAL);
}

ITCHParser::~ITCHParser() {
    if (data_ && size_ > 0) {
        ::munmap(data_, size_);
    }
}

ITCHParser::ITCHParser(ITCHParser&& other) noexcept
    : data_(other.data_), size_(other.size_)
{
    other.data_ = nullptr;
    other.size_ = 0;
}

ITCHParser& ITCHParser::operator=(ITCHParser&& other) noexcept {
    if (this != &other) {
        if (data_ && size_ > 0) {
            ::munmap(data_, size_);
        }
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

// ─────────────────────────────────────────────────────────────────────────────
// Endianness Helpers
// ─────────────────────────────────────────────────────────────────────────────

uint16_t ITCHParser::read_be16(const uint8_t* p) noexcept {
    uint16_t val;
    std::memcpy(&val, p, sizeof(val));
    return __builtin_bswap16(val);
}

uint32_t ITCHParser::read_be32(const uint8_t* p) noexcept {
    uint32_t val;
    std::memcpy(&val, p, sizeof(val));
    return __builtin_bswap32(val);
}

uint64_t ITCHParser::read_be64(const uint8_t* p) noexcept {
    uint64_t val;
    std::memcpy(&val, p, sizeof(val));
    return __builtin_bswap64(val);
}

uint64_t ITCHParser::read_be48(const uint8_t* p) noexcept {
    // ITCH timestamps are 6 bytes, big-endian. We read into the high bytes
    // of a uint64_t and shift down.
    //
    // Byte layout: [B5 B4 B3 B2 B1 B0] where B5 is MSB.
    // We assemble manually to avoid unaligned 8-byte reads crossing the
    // boundary of the 6-byte field.
    uint64_t val = 0;
    val |= static_cast<uint64_t>(p[0]) << 40;
    val |= static_cast<uint64_t>(p[1]) << 32;
    val |= static_cast<uint64_t>(p[2]) << 24;
    val |= static_cast<uint64_t>(p[3]) << 16;
    val |= static_cast<uint64_t>(p[4]) << 8;
    val |= static_cast<uint64_t>(p[5]);
    return val;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-Message Parsers
// ─────────────────────────────────────────────────────────────────────────────

void ITCHParser::parse_add_order(const uint8_t* p, Order& order) noexcept {
    // Message type 'A' — Add Order (no MPID)
    //
    // We map this to a Limit order that will be inserted into the book.
    //
    // Field offsets (from start of message payload, after the 2-byte length):
    //   [0]      type = 'A'          (1 byte, already checked by caller)
    //   [5..10]  timestamp            (6 bytes, nanoseconds since midnight)
    //   [11..18] order_reference      (8 bytes)
    //   [19]     buy_sell_indicator    (1 byte, 'B' or 'S')
    //   [20..23] shares               (4 bytes)
    //   [24..31] stock                (8 bytes, right-padded with spaces)
    //   [32..35] price                (4 bytes, fixed-point 4 decimal places)

    order.order_type   = OrderType::Limit;
    order.timestamp_ns = read_be48(p + 5);
    order.order_id     = read_be64(p + 11);
    order.side         = (p[19] == 'B') ? Side::Bid : Side::Ask;
    order.quantity     = static_cast<int64_t>(read_be32(p + 20));
    // Symbol stored at p+24 (8 bytes): we ignore it — single-symbol simulation

    // ITCH price is a 4-byte integer with 4 implied decimal places.
    // E.g. 150.2500 is stored as 1502500.
    // We keep this fixed-point representation as-is.
    order.price = static_cast<int64_t>(read_be32(p + 32));
}

void ITCHParser::parse_delete_order(const uint8_t* p, Order& order) noexcept {
    // Message type 'D' — Order Delete
    //
    // We map this to a Cancel order. Only the order reference and timestamp
    // are present; the matching engine must look up the original order by ID.
    //
    //   [5..10]  timestamp            (6 bytes)
    //   [11..18] order_reference      (8 bytes)

    order.order_type   = OrderType::Cancel;
    order.timestamp_ns = read_be48(p + 5);
    order.order_id     = read_be64(p + 11);
    order.price        = 0;
    order.quantity     = 0;
    order.side         = Side::Bid;  // Unknown — ME must look up; use Bid as default
}

void ITCHParser::parse_executed(const uint8_t* p, Order& order) noexcept {
    // Message type 'E' — Order Executed
    //
    // An existing resting order was (partially) executed. We model this as
    // a Market order so the matching engine recognizes it as a fill event.
    //
    //   [5..10]  timestamp            (6 bytes)
    //   [11..18] order_reference      (8 bytes)
    //   [19..22] executed_shares      (4 bytes)
    //   [23..30] match_number         (8 bytes, used as trade_id)

    order.order_type   = OrderType::Market;
    order.timestamp_ns = read_be48(p + 5);
    order.order_id     = read_be64(p + 11);
    order.quantity     = static_cast<int64_t>(read_be32(p + 19));
    order.price        = 0;  // Execution price not in 'E' message
    order.side         = Side::Bid;  // Unknown — ME must look up; use Bid as default
}

void ITCHParser::parse_trade(const uint8_t* p, Order& order) noexcept {
    // Message type 'P' — Trade (non-cross)
    //
    // A trade that is not the result of a cross event. Contains full
    // price and symbol information.
    //
    //   [5..10]  timestamp            (6 bytes)
    //   [11..18] order_reference      (8 bytes)
    //   [19]     buy_sell_indicator    (1 byte)
    //   [20..23] shares               (4 bytes)
    //   [24..31] stock                (8 bytes)
    //   [32..35] price                (4 bytes, fixed-point)
    //   [36..43] match_number         (8 bytes)

    order.order_type   = OrderType::Market;
    order.timestamp_ns = read_be48(p + 5);
    order.order_id     = read_be64(p + 11);
    order.side         = (p[19] == 'B') ? Side::Bid : Side::Ask;
    order.quantity     = static_cast<int64_t>(read_be32(p + 20));
    // Symbol at p+24 ignored — single-symbol simulation
    order.price        = static_cast<int64_t>(read_be32(p + 32));
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Parse Loop
// ─────────────────────────────────────────────────────────────────────────────

std::vector<Order> ITCHParser::parse_all() const {
    // Pre-allocate for typical ITCH files. A full trading day can have
    // ~300M messages, but most are types we skip. Reserve conservatively.
    std::vector<Order> orders;
    orders.reserve(1'000'000);

    std::size_t offset = 0;

    while (offset + 2 < size_) {
        // ── Read 2-byte message length ───────────────────────────────────
        // This length does NOT include the 2 length bytes themselves.
        const uint16_t msg_len = read_be16(data_ + offset);
        offset += 2;

        // Bounds check: ensure we have enough data for the full message.
        if (offset + msg_len > size_) {
            break; // Truncated message at end of file — stop gracefully
        }

        // Pointer to the start of the message payload
        const uint8_t* msg = data_ + offset;

        // ── Dispatch on message type (first byte) ────────────────────────
        const char msg_type = static_cast<char>(msg[0]);

        Order order{};

        switch (msg_type) {
            case 'A':
                // Add Order: requires at least 36 bytes
                if (msg_len >= 36) {
                    parse_add_order(msg, order);
                    orders.push_back(order);
                }
                break;

            case 'D':
                // Order Delete: requires at least 19 bytes
                if (msg_len >= 19) {
                    parse_delete_order(msg, order);
                    orders.push_back(order);
                }
                break;

            case 'E':
                // Order Executed: requires at least 31 bytes
                if (msg_len >= 31) {
                    parse_executed(msg, order);
                    orders.push_back(order);
                }
                break;

            case 'P':
                // Trade (non-cross): requires at least 44 bytes
                if (msg_len >= 44) {
                    parse_trade(msg, order);
                    orders.push_back(order);
                }
                break;

            default:
                // Skip all other message types (S, R, H, Y, L, V, W, K,
                // J, h, Q, B, I, N, etc.)
                break;
        }

        // Advance past this message
        offset += msg_len;
    }

    return orders;
}

} // namespace hft
