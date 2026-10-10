#include "quill/matching_engine.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace quill {

MatchingEngine::MatchingEngine(EventListener& listener, Clock& clock)
    : listener_(&listener), clock_(&clock) {}

InstrumentId MatchingEngine::add_instrument(InstrumentSpec spec) {
    if (spec.symbol.empty()) {
        throw std::invalid_argument("instrument symbol must not be empty");
    }
    if (spec.tick_size <= 0 || spec.lot_size <= 0 || spec.max_order_quantity <= 0) {
        throw std::invalid_argument("instrument " + spec.symbol +
                                    ": tick, lot and max quantity must be positive");
    }
    if (find_instrument(spec.symbol)) {
        throw std::invalid_argument("duplicate instrument symbol " + spec.symbol);
    }
    const auto id = static_cast<InstrumentId>(books_.size() + 1);
    books_.push_back(std::make_unique<OrderBook>(id, std::move(spec), *listener_, match_sequence_));
    return id;
}

std::optional<InstrumentId> MatchingEngine::find_instrument(std::string_view symbol) const {
    for (const auto& book : books_) {
        if (book->spec().symbol == symbol) {
            return book->id();
        }
    }
    return std::nullopt;
}

const OrderBook* MatchingEngine::book(InstrumentId id) const noexcept {
    return (id >= 1 && id <= books_.size()) ? books_[id - 1].get() : nullptr;
}

OrderBook* MatchingEngine::lookup(InstrumentId id) noexcept {
    return (id >= 1 && id <= books_.size()) ? books_[id - 1].get() : nullptr;
}

void MatchingEngine::process(const Command& command) {
    if (const auto* order = std::get_if<NewOrderRequest>(&command.payload)) {
        submit(*order);
    } else if (const auto* cancel_request = std::get_if<CancelRequest>(&command.payload)) {
        cancel(*cancel_request);
    } else if (const auto* replace_request = std::get_if<ReplaceRequest>(&command.payload)) {
        replace(*replace_request);
    }
}

OrderId MatchingEngine::submit(const NewOrderRequest& request) {
    ++commands_;
    OrderBook* book = lookup(request.instrument);
    if (book == nullptr) {
        reject_unknown_instrument(request.instrument, RequestKind::NewOrder, kInvalidOrderId,
                                  request.client_tag);
        return kInvalidOrderId;
    }
    const bool client_assigned = request.order_id != kInvalidOrderId;
    const OrderId id = client_assigned ? request.order_id : next_order_id_;
    if (!book->submit(request, id, clock_->now())) {
        return kInvalidOrderId;
    }
    // Keep the automatic sequence ahead of any id a client chose so they never collide.
    if (id >= next_order_id_) {
        next_order_id_ = id + 1;
    }
    return id;
}

void MatchingEngine::cancel(const CancelRequest& request) {
    ++commands_;
    if (OrderBook* book = lookup(request.instrument)) {
        book->cancel(request, clock_->now());
    } else {
        reject_unknown_instrument(request.instrument, RequestKind::Cancel, request.order_id,
                                  request.client_tag);
    }
}

void MatchingEngine::replace(const ReplaceRequest& request) {
    ++commands_;
    if (OrderBook* book = lookup(request.instrument)) {
        book->replace(request, clock_->now());
    } else {
        reject_unknown_instrument(request.instrument, RequestKind::Replace, request.order_id,
                                  request.client_tag);
    }
}

void MatchingEngine::reject_unknown_instrument(InstrumentId id, RequestKind kind, OrderId order_id,
                                               std::uint64_t client_tag) {
    listener_->on_rejected(OrderRejected{id, order_id, client_tag, kind,
                                         RejectReason::UnknownInstrument, clock_->now()});
}

}  // namespace quill
