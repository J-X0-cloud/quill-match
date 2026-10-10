#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace quill {

/// Prices are fixed-point integers with four implied decimal places, the same scale used
/// by ITCH-style feeds. A price of 1234500 therefore means 123.45.
using Price = std::int64_t;
/// Quantities are whole units (shares, contracts). Signed so that arithmetic on
/// differences never silently wraps.
using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using InstrumentId = std::uint32_t;
using ParticipantId = std::uint32_t;
/// Nanoseconds on whatever clock the engine was constructed with.
using Timestamp = std::uint64_t;
using MatchId = std::uint64_t;

inline constexpr Price kPriceScale = 10'000;
inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr Price kMaxPrice = std::numeric_limits<std::int32_t>::max();
inline constexpr Quantity kMaxQuantity = std::numeric_limits<std::int32_t>::max();

enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OrderType : std::uint8_t { Limit, Market };

enum class TimeInForce : std::uint8_t {
    /// Rests on the book until filled or cancelled.
    GoodTillCancel,
    /// Matches what it can immediately; the remainder is cancelled.
    ImmediateOrCancel,
    /// Fills completely and immediately or does nothing at all.
    FillOrKill,
};

/// What to do when an incoming order would trade against a resting order that belongs to
/// the same participant.
enum class SelfTradePrevention : std::uint8_t {
    None,
    CancelIncoming,
    CancelResting,
    CancelBoth,
};

enum class RejectReason : std::uint8_t {
    None,
    UnknownInstrument,
    UnknownOrder,
    InvalidQuantity,
    InvalidPrice,
    PriceNotOnTick,
    QuantityNotOnLot,
    InvalidDisplayQuantity,
    InvalidTimeInForce,
    PostOnlyWouldCross,
    FillOrKillUnfillable,
    NoLiquidity,
    DuplicateOrderId,
    QuantityBelowExecuted,
    PriceOutsideCollar,
};

enum class CancelReason : std::uint8_t {
    UserRequested,
    ImmediateOrCancelRemainder,
    MarketOrderRemainder,
    SelfTradePrevention,
};

[[nodiscard]] constexpr std::string_view to_string(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

[[nodiscard]] constexpr std::string_view to_string(OrderType type) noexcept {
    return type == OrderType::Limit ? "LIMIT" : "MARKET";
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::GoodTillCancel:
            return "GTC";
        case TimeInForce::ImmediateOrCancel:
            return "IOC";
        case TimeInForce::FillOrKill:
            return "FOK";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None:
            return "none";
        case RejectReason::UnknownInstrument:
            return "unknown instrument";
        case RejectReason::UnknownOrder:
            return "unknown order";
        case RejectReason::InvalidQuantity:
            return "invalid quantity";
        case RejectReason::InvalidPrice:
            return "invalid price";
        case RejectReason::PriceNotOnTick:
            return "price not on tick";
        case RejectReason::QuantityNotOnLot:
            return "quantity not a multiple of lot size";
        case RejectReason::InvalidDisplayQuantity:
            return "invalid display quantity";
        case RejectReason::InvalidTimeInForce:
            return "invalid time in force for order type";
        case RejectReason::PostOnlyWouldCross:
            return "post-only order would cross";
        case RejectReason::FillOrKillUnfillable:
            return "fill-or-kill order cannot be filled";
        case RejectReason::NoLiquidity:
            return "no liquidity";
        case RejectReason::DuplicateOrderId:
            return "duplicate order id";
        case RejectReason::QuantityBelowExecuted:
            return "replace quantity not above executed quantity";
        case RejectReason::PriceOutsideCollar:
            return "price outside collar";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(CancelReason reason) noexcept {
    switch (reason) {
        case CancelReason::UserRequested:
            return "user requested";
        case CancelReason::ImmediateOrCancelRemainder:
            return "IOC remainder";
        case CancelReason::MarketOrderRemainder:
            return "market order remainder";
        case CancelReason::SelfTradePrevention:
            return "self-trade prevention";
    }
    return "?";
}

/// Returns true if a buy at `bid` and a sell at `ask` are marketable against each other.
[[nodiscard]] constexpr bool crosses(Price bid, Price ask) noexcept { return bid >= ask; }

/// True if `price` is at least as aggressive as `limit` for an order on `side`.
[[nodiscard]] constexpr bool price_acceptable(Side side, Price price, Price limit) noexcept {
    return side == Side::Buy ? price <= limit : price >= limit;
}

}  // namespace quill
