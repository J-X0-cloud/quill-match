#pragma once

#include <cstdint>
#include <vector>

#include "quill/types.hpp"

namespace quill {

/// Which kind of request a rejection refers to.
enum class RequestKind : std::uint8_t { NewOrder, Cancel, Replace };

// ---------------------------------------------------------------------------------------
// Order lifecycle events: the private execution-report stream for order owners.
// ---------------------------------------------------------------------------------------

struct OrderAccepted {
    InstrumentId instrument = 0;
    OrderId order_id = kInvalidOrderId;
    std::uint64_t client_tag = 0;
    ParticipantId participant = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GoodTillCancel;
    Price price = 0;
    Quantity quantity = 0;
    Timestamp timestamp = 0;
};

struct OrderRejected {
    InstrumentId instrument = 0;
    /// The order the request referred to; zero for a rejected new order that was never
    /// assigned an id.
    OrderId order_id = kInvalidOrderId;
    std::uint64_t client_tag = 0;
    RequestKind request = RequestKind::NewOrder;
    RejectReason reason = RejectReason::None;
    Timestamp timestamp = 0;
};

struct Trade {
    InstrumentId instrument = 0;
    MatchId match_id = 0;
    OrderId aggressor_id = kInvalidOrderId;
    OrderId resting_id = kInvalidOrderId;
    ParticipantId aggressor_participant = 0;
    ParticipantId resting_participant = 0;
    Side aggressor_side = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;
    /// Quantity left open on each order after this fill.
    Quantity aggressor_leaves = 0;
    Quantity resting_leaves = 0;
    Timestamp timestamp = 0;
};

struct OrderCancelled {
    InstrumentId instrument = 0;
    OrderId order_id = kInvalidOrderId;
    std::uint64_t client_tag = 0;
    Quantity cancelled_quantity = 0;
    CancelReason reason = CancelReason::UserRequested;
    Timestamp timestamp = 0;
};

struct OrderReplaced {
    InstrumentId instrument = 0;
    OrderId order_id = kInvalidOrderId;
    std::uint64_t client_tag = 0;
    Price price = 0;
    Quantity quantity = 0;
    Quantity leaves = 0;
    /// True when the change was a pure size reduction that kept queue position.
    bool priority_kept = false;
    Timestamp timestamp = 0;
};

// ---------------------------------------------------------------------------------------
// Market data events: the public, order-level view of the displayed book. Reserve
// quantity of iceberg orders never appears here; each refill shows up as a fresh add.
// ---------------------------------------------------------------------------------------

struct BookOrderAdded {
    InstrumentId instrument = 0;
    OrderId order_id = kInvalidOrderId;
    Side side = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;
    Timestamp timestamp = 0;
};

struct BookOrderExecuted {
    InstrumentId instrument = 0;
    OrderId order_id = kInvalidOrderId;
    Quantity quantity = 0;
    MatchId match_id = 0;
    Timestamp timestamp = 0;
};

/// Partial reduction of displayed quantity without execution.
struct BookOrderReduced {
    InstrumentId instrument = 0;
    OrderId order_id = kInvalidOrderId;
    Quantity quantity = 0;
    Timestamp timestamp = 0;
};

struct BookOrderDeleted {
    InstrumentId instrument = 0;
    OrderId order_id = kInvalidOrderId;
    Timestamp timestamp = 0;
};

/// Receives everything the engine produces. Every callback has an empty default so a
/// consumer only overrides what it cares about. Callbacks run synchronously on the
/// matching thread, so implementations must be fast and must not call back into the
/// engine.
class EventListener {
  public:
    EventListener() = default;
    EventListener(const EventListener&) = default;
    EventListener& operator=(const EventListener&) = default;
    EventListener(EventListener&&) = default;
    EventListener& operator=(EventListener&&) = default;
    virtual ~EventListener() = default;

    virtual void on_accepted(const OrderAccepted& /*event*/) {}
    virtual void on_rejected(const OrderRejected& /*event*/) {}
    virtual void on_trade(const Trade& /*event*/) {}
    virtual void on_cancelled(const OrderCancelled& /*event*/) {}
    virtual void on_replaced(const OrderReplaced& /*event*/) {}

    virtual void on_book_add(const BookOrderAdded& /*event*/) {}
    virtual void on_book_execute(const BookOrderExecuted& /*event*/) {}
    virtual void on_book_reduce(const BookOrderReduced& /*event*/) {}
    virtual void on_book_delete(const BookOrderDeleted& /*event*/) {}
};

/// Forwards every event to a list of listeners, in registration order.
class EventFanout final : public EventListener {
  public:
    void add(EventListener& listener) { listeners_.push_back(&listener); }

    void on_accepted(const OrderAccepted& e) override { forward(&EventListener::on_accepted, e); }
    void on_rejected(const OrderRejected& e) override { forward(&EventListener::on_rejected, e); }
    void on_trade(const Trade& e) override { forward(&EventListener::on_trade, e); }
    void on_cancelled(const OrderCancelled& e) override {
        forward(&EventListener::on_cancelled, e);
    }
    void on_replaced(const OrderReplaced& e) override { forward(&EventListener::on_replaced, e); }
    void on_book_add(const BookOrderAdded& e) override { forward(&EventListener::on_book_add, e); }
    void on_book_execute(const BookOrderExecuted& e) override {
        forward(&EventListener::on_book_execute, e);
    }
    void on_book_reduce(const BookOrderReduced& e) override {
        forward(&EventListener::on_book_reduce, e);
    }
    void on_book_delete(const BookOrderDeleted& e) override {
        forward(&EventListener::on_book_delete, e);
    }

  private:
    template <typename Event>
    void forward(void (EventListener::*callback)(const Event&), const Event& event) {
        for (EventListener* listener : listeners_) {
            (listener->*callback)(event);
        }
    }

    std::vector<EventListener*> listeners_;
};

}  // namespace quill
