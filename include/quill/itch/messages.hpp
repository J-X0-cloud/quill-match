#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "quill/types.hpp"

/// ITCH-style binary market data.
///
/// The wire format follows the shape of NASDAQ TotalView-ITCH 5.0: every message starts
/// with a one-byte type, a two-byte stock locate code, a two-byte tracking number and a
/// six-byte nanoseconds-since-midnight timestamp, and all integers are big-endian. Prices
/// are four-byte unsigned integers with four implied decimals. The add, execute, cancel,
/// delete and replace messages have the same layout and size as their ITCH 5.0
/// counterparts. The stock directory message is a reduced variant that carries only what
/// a book builder needs (symbol, round lot and tick size).
///
/// Files are a sequence of frames, each a two-byte big-endian length followed by that many
/// message bytes, which is the framing used by NASDAQ's historical ITCH files.
namespace quill::itch {

using Symbol = std::array<char, 8>;

/// Space-padded 8-character symbol, as ITCH encodes it. Longer input is truncated.
[[nodiscard]] Symbol make_symbol(std::string_view text) noexcept;
/// Symbol text with trailing padding removed.
[[nodiscard]] std::string symbol_text(const Symbol& symbol);

namespace msg_type {
inline constexpr char kSystemEvent = 'S';
inline constexpr char kStockDirectory = 'R';
inline constexpr char kAddOrder = 'A';
inline constexpr char kOrderExecuted = 'E';
inline constexpr char kOrderCancel = 'X';
inline constexpr char kOrderDelete = 'D';
inline constexpr char kOrderReplace = 'U';
}  // namespace msg_type

namespace system_event {
inline constexpr char kStartOfMessages = 'O';
inline constexpr char kStartOfMarketHours = 'Q';
inline constexpr char kEndOfMarketHours = 'M';
inline constexpr char kEndOfMessages = 'C';
}  // namespace system_event

inline constexpr std::size_t kHeaderSize = 11;
inline constexpr std::uint64_t kNanosPerDay = 86'400ULL * 1'000'000'000ULL;

struct Header {
    std::uint16_t locate = 0;
    std::uint16_t tracking = 0;
    /// Nanoseconds since midnight; only the low 48 bits travel on the wire.
    std::uint64_t timestamp = 0;

    friend bool operator==(const Header&, const Header&) = default;
};

struct SystemEvent {
    static constexpr char kType = msg_type::kSystemEvent;
    static constexpr std::size_t kSize = kHeaderSize + 1;
    Header header;
    char event_code = system_event::kStartOfMessages;

    friend bool operator==(const SystemEvent&, const SystemEvent&) = default;
};

struct StockDirectory {
    static constexpr char kType = msg_type::kStockDirectory;
    static constexpr std::size_t kSize = kHeaderSize + 8 + 4 + 4;
    Header header;
    Symbol stock{};
    std::uint32_t round_lot_size = 0;
    std::uint32_t tick_size = 0;

    friend bool operator==(const StockDirectory&, const StockDirectory&) = default;
};

struct AddOrder {
    static constexpr char kType = msg_type::kAddOrder;
    static constexpr std::size_t kSize = kHeaderSize + 8 + 1 + 4 + 8 + 4;  // 36
    Header header;
    std::uint64_t order_ref = 0;
    Side side = Side::Buy;
    std::uint32_t shares = 0;
    Symbol stock{};
    std::uint32_t price = 0;

    friend bool operator==(const AddOrder&, const AddOrder&) = default;
};

struct OrderExecuted {
    static constexpr char kType = msg_type::kOrderExecuted;
    static constexpr std::size_t kSize = kHeaderSize + 8 + 4 + 8;  // 31
    Header header;
    std::uint64_t order_ref = 0;
    std::uint32_t executed_shares = 0;
    std::uint64_t match_number = 0;

    friend bool operator==(const OrderExecuted&, const OrderExecuted&) = default;
};

struct OrderCancel {
    static constexpr char kType = msg_type::kOrderCancel;
    static constexpr std::size_t kSize = kHeaderSize + 8 + 4;  // 23
    Header header;
    std::uint64_t order_ref = 0;
    std::uint32_t cancelled_shares = 0;

    friend bool operator==(const OrderCancel&, const OrderCancel&) = default;
};

struct OrderDelete {
    static constexpr char kType = msg_type::kOrderDelete;
    static constexpr std::size_t kSize = kHeaderSize + 8;  // 19
    Header header;
    std::uint64_t order_ref = 0;

    friend bool operator==(const OrderDelete&, const OrderDelete&) = default;
};

struct OrderReplace {
    static constexpr char kType = msg_type::kOrderReplace;
    static constexpr std::size_t kSize = kHeaderSize + 8 + 8 + 4 + 4;  // 35
    Header header;
    std::uint64_t original_ref = 0;
    std::uint64_t new_ref = 0;
    std::uint32_t shares = 0;
    std::uint32_t price = 0;

    friend bool operator==(const OrderReplace&, const OrderReplace&) = default;
};

}  // namespace quill::itch
