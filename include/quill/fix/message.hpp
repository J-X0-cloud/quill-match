#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quill/types.hpp"

/// A FIX-like tag=value text protocol.
///
/// Messages use the standard FIX envelope (BeginString 8, BodyLength 9, MsgType 35 first,
/// CheckSum 10 last) and standard tag numbers for the order-entry subset the gateway
/// understands. The delimiter is configurable so that '|' can stand in for SOH when a
/// human is typing messages.
namespace quill::fix {

inline constexpr char kSoh = '\x01';
inline constexpr std::string_view kBeginString = "FIX.4.4";

namespace tag {
inline constexpr int kAvgPx = 6;
inline constexpr int kBeginString = 8;
inline constexpr int kBodyLength = 9;
inline constexpr int kCheckSum = 10;
inline constexpr int kClOrdId = 11;
inline constexpr int kCumQty = 14;
inline constexpr int kExecId = 17;
inline constexpr int kExecInst = 18;
inline constexpr int kLastPx = 31;
inline constexpr int kLastQty = 32;
inline constexpr int kMsgSeqNum = 34;
inline constexpr int kMsgType = 35;
inline constexpr int kOrderId = 37;
inline constexpr int kOrderQty = 38;
inline constexpr int kOrdStatus = 39;
inline constexpr int kOrdType = 40;
inline constexpr int kOrigClOrdId = 41;
inline constexpr int kRefSeqNum = 45;
inline constexpr int kPrice = 44;
inline constexpr int kSenderCompId = 49;
inline constexpr int kSendingTime = 52;
inline constexpr int kSide = 54;
inline constexpr int kSymbol = 55;
inline constexpr int kTargetCompId = 56;
inline constexpr int kText = 58;
inline constexpr int kTimeInForce = 59;
inline constexpr int kHeartBtInt = 108;
inline constexpr int kCxlRejReason = 102;
inline constexpr int kOrdRejReason = 103;
inline constexpr int kTestReqId = 112;
inline constexpr int kExecType = 150;
inline constexpr int kLeavesQty = 151;
inline constexpr int kRefTagId = 371;
inline constexpr int kRefMsgType = 372;
inline constexpr int kSessionRejectReason = 373;
inline constexpr int kCxlRejResponseTo = 434;
inline constexpr int kDisplayQty = 1138;
inline constexpr int kSelfMatchPrevention = 2964;
}  // namespace tag

namespace msg {
inline constexpr std::string_view kHeartbeat = "0";
inline constexpr std::string_view kTestRequest = "1";
inline constexpr std::string_view kReject = "3";
inline constexpr std::string_view kLogout = "5";
inline constexpr std::string_view kExecutionReport = "8";
inline constexpr std::string_view kOrderCancelReject = "9";
inline constexpr std::string_view kLogon = "A";
inline constexpr std::string_view kNewOrderSingle = "D";
inline constexpr std::string_view kOrderCancelRequest = "F";
inline constexpr std::string_view kOrderCancelReplaceRequest = "G";
}  // namespace msg

enum class ParseError : std::uint8_t {
    Empty,
    MalformedField,
    BadTagNumber,
    MissingBeginString,
    MissingBodyLength,
    MissingMsgType,
    BodyLengthMismatch,
    MissingCheckSum,
    CheckSumMismatch,
    FieldsAfterCheckSum,
};

[[nodiscard]] std::string_view to_string(ParseError error) noexcept;

struct Field {
    int tag = 0;
    std::string_view value;
};

/// A parsed message. Owns a copy of its raw text; fields are stored as offsets into it,
/// so copies and moves are always safe.
class Message {
  public:
    /// All fields in wire order, as views into this message's storage.
    [[nodiscard]] std::vector<Field> fields() const;
    [[nodiscard]] std::size_t field_count() const noexcept { return slots_.size(); }
    [[nodiscard]] std::string_view msg_type() const noexcept;

    /// First occurrence of `tag`, if present.
    [[nodiscard]] std::optional<std::string_view> get(int tag) const noexcept;
    [[nodiscard]] bool has(int tag) const noexcept { return get(tag).has_value(); }
    /// Parses the value as a base-10 integer; nullopt if the tag is absent or the value
    /// is not an integer.
    [[nodiscard]] std::optional<std::int64_t> get_int(int tag) const noexcept;

    struct Result;
    /// Parses one complete message. With `strict` the envelope is enforced (BodyLength
    /// and CheckSum must be present and correct); without it they are optional, which is
    /// convenient for hand-written input.
    [[nodiscard]] static Result parse(std::string_view raw, char delimiter = kSoh,
                                      bool strict = true);

  private:
    struct Slot {
        int tag = 0;
        std::uint32_t start = 0;   // first byte of "tag="
        std::uint32_t offset = 0;  // first byte of the value
        std::uint32_t length = 0;
    };

    [[nodiscard]] std::string_view value(const Slot& slot) const noexcept {
        return std::string_view(raw_).substr(slot.offset, slot.length);
    }

    std::string raw_;
    std::vector<Slot> slots_;
};

struct Message::Result {
    std::optional<Message> message;
    std::optional<ParseError> error;
    /// Tag that caused the error, when meaningful.
    int error_tag = 0;
};

/// Builds an outbound message, filling in BodyLength and CheckSum.
class Builder {
  public:
    explicit Builder(std::string_view msg_type, char delimiter = kSoh);

    Builder& add(int tag, std::string_view value);
    Builder& add(int tag, std::int64_t value);
    Builder& add(int tag, char value);

    /// Returns the finished message text.
    [[nodiscard]] std::string finish(std::string_view begin_string = kBeginString) const;

  private:
    char delimiter_;
    std::string body_;  // everything from 35= through the last field's delimiter
};

/// Sum of bytes modulo 256, as used by tag 10.
[[nodiscard]] unsigned checksum(std::string_view bytes) noexcept;

/// Parses a decimal price such as "123.4567" into fixed-point units. At most four
/// decimals (extra trailing zeros are fine); no sign, exponent or whitespace.
[[nodiscard]] std::optional<Price> parse_price(std::string_view text) noexcept;
/// Formats a fixed-point price with the fewest decimals that represent it exactly
/// (at least two), e.g. 1234500 becomes "123.45".
[[nodiscard]] std::string format_price(Price price);
/// Same for a fractional value with arbitrary scale, used for average prices.
[[nodiscard]] std::string format_decimal(double value, int max_decimals = 6);

/// FIX UTCTimestamp with millisecond precision: YYYYMMDD-HH:MM:SS.sss
[[nodiscard]] std::string format_utc_timestamp(Timestamp nanos_since_epoch);

}  // namespace quill::fix
