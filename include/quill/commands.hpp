#pragma once

#include <cstdint>
#include <type_traits>
#include <variant>

#include "quill/types.hpp"

namespace quill {

struct NewOrderRequest {
    InstrumentId instrument = 0;
    /// Zero lets the engine assign the next id; a non-zero value is used verbatim and is
    /// rejected if it collides with a live order.
    OrderId order_id = kInvalidOrderId;
    std::uint64_t client_tag = 0;
    ParticipantId participant = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GoodTillCancel;
    SelfTradePrevention stp = SelfTradePrevention::None;
    bool post_only = false;
    /// Ignored for market orders.
    Price price = 0;
    Quantity quantity = 0;
    /// Iceberg peak size. Zero displays the full quantity.
    Quantity display_quantity = 0;
};

struct CancelRequest {
    InstrumentId instrument = 0;
    OrderId order_id = kInvalidOrderId;
    std::uint64_t client_tag = 0;
};

/// Cancel/replace. `quantity` is the new *total* order quantity, including anything that
/// has already executed, mirroring FIX OrderQty semantics on OrderCancelReplaceRequest.
struct ReplaceRequest {
    InstrumentId instrument = 0;
    OrderId order_id = kInvalidOrderId;
    std::uint64_t client_tag = 0;
    Price price = 0;
    Quantity quantity = 0;
};

using CommandPayload = std::variant<NewOrderRequest, CancelRequest, ReplaceRequest>;

/// The unit of work carried through the ingress queue.
struct Command {
    CommandPayload payload;
    /// When the command was produced; used to measure queueing plus matching latency.
    Timestamp enqueued_at = 0;
};

static_assert(std::is_trivially_copyable_v<Command>,
              "commands must be trivially copyable to travel through the SPSC ring");

}  // namespace quill
