#include "quill/itch/publisher.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace quill::itch {

namespace {

std::uint32_t to_wire_u32(std::int64_t value, const char* what) {
    if (value < 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::out_of_range(std::string(what) + " does not fit the ITCH wire format");
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace

Header Publisher::header(InstrumentId id, Timestamp t) noexcept {
    // Tracking numbers are a per-feed rolling counter, which is all a single-engine feed
    // needs to let a consumer spot dropped messages.
    return Header{static_cast<std::uint16_t>(id), tracking_++, t};
}

const Symbol& Publisher::symbol(InstrumentId id) const {
    if (id >= symbols_.size()) {
        throw std::out_of_range("market data for unregistered instrument " + std::to_string(id));
    }
    return symbols_[id];
}

void Publisher::start_of_messages(Timestamp now) {
    writer_.write(SystemEvent{header(0, now), system_event::kStartOfMessages});
}

void Publisher::end_of_messages(Timestamp now) {
    writer_.write(SystemEvent{header(0, now), system_event::kEndOfMessages});
}

void Publisher::register_instrument(InstrumentId id, const InstrumentSpec& spec, Timestamp now) {
    if (id > std::numeric_limits<std::uint16_t>::max()) {
        throw std::out_of_range("instrument id exceeds the 16-bit locate code range");
    }
    if (symbols_.size() <= id) {
        symbols_.resize(id + 1, make_symbol(""));
    }
    symbols_[id] = make_symbol(spec.symbol);
    writer_.write(StockDirectory{header(id, now), symbols_[id],
                                 to_wire_u32(spec.lot_size, "lot size"),
                                 to_wire_u32(spec.tick_size, "tick size")});
}

void Publisher::on_book_add(const BookOrderAdded& e) {
    writer_.write(AddOrder{header(e.instrument, e.timestamp), e.order_id, e.side,
                           to_wire_u32(e.quantity, "quantity"), symbol(e.instrument),
                           to_wire_u32(e.price, "price")});
}

void Publisher::on_book_execute(const BookOrderExecuted& e) {
    writer_.write(OrderExecuted{header(e.instrument, e.timestamp), e.order_id,
                                to_wire_u32(e.quantity, "quantity"), e.match_id});
}

void Publisher::on_book_reduce(const BookOrderReduced& e) {
    writer_.write(OrderCancel{header(e.instrument, e.timestamp), e.order_id,
                              to_wire_u32(e.quantity, "quantity")});
}

void Publisher::on_book_delete(const BookOrderDeleted& e) {
    writer_.write(OrderDelete{header(e.instrument, e.timestamp), e.order_id});
}

void Publisher::flush(std::ostream& out) {
    const auto bytes = writer_.data();
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    writer_.clear();
}

}  // namespace quill::itch
