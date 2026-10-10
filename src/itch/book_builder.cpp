#include "quill/itch/book_builder.hpp"

#include <algorithm>

namespace quill::itch {

BookBuilder::InstrumentState& BookBuilder::state(std::uint16_t locate) {
    if (instruments_.size() <= locate) {
        instruments_.resize(static_cast<std::size_t>(locate) + 1);
    }
    return instruments_[locate];
}

const BookBuilder::InstrumentState* BookBuilder::instrument(std::uint16_t locate) const noexcept {
    return locate < instruments_.size() ? &instruments_[locate] : nullptr;
}

std::optional<std::uint16_t> BookBuilder::find(const std::string& symbol) const {
    for (const std::uint16_t locate : locates_) {
        if (instruments_[locate].symbol == symbol) {
            return locate;
        }
    }
    return std::nullopt;
}

void BookBuilder::on_message(const SystemEvent& m) {
    ++stats_.system_events;
    if (m.event_code == system_event::kEndOfMessages) {
        ended_ = true;
    }
}

void BookBuilder::on_message(const StockDirectory& m) {
    ++stats_.directory;
    InstrumentState& s = state(m.header.locate);
    if (s.symbol.empty()) {
        locates_.push_back(m.header.locate);
    }
    s.symbol = symbol_text(m.stock);
    s.round_lot = m.round_lot_size;
    s.tick_size = m.tick_size;
}

void BookBuilder::add_to_level(const Entry& entry) {
    InstrumentState& s = state(entry.locate);
    Level& level = entry.side == Side::Buy ? s.bids[entry.price] : s.asks[entry.price];
    level.quantity += entry.quantity;
    ++level.orders;
}

void BookBuilder::take_from_level(const Entry& entry, Quantity quantity, bool remove_order) {
    InstrumentState& s = state(entry.locate);
    const auto apply = [&](auto& side) {
        const auto it = side.find(entry.price);
        if (it == side.end()) {
            return;
        }
        it->second.quantity -= quantity;
        if (remove_order) {
            --it->second.orders;
        }
        if (it->second.orders == 0) {
            side.erase(it);
        }
    };
    if (entry.side == Side::Buy) {
        apply(s.bids);
    } else {
        apply(s.asks);
    }
}

void BookBuilder::insert(std::uint64_t ref, const Entry& entry) {
    if (!orders_.insert(ref, entry)) {
        ++stats_.anomalies;
        return;
    }
    add_to_level(entry);
}

void BookBuilder::reduce(std::uint64_t ref, Quantity quantity, bool executed) {
    Entry* entry = orders_.find(ref);
    if (entry == nullptr || quantity > entry->quantity) {
        ++stats_.anomalies;
        return;
    }
    if (executed) {
        InstrumentState& s = state(entry->locate);
        ++s.executions;
        s.executed_volume += quantity;
        s.last_price = entry->price;
    }
    const bool gone = quantity == entry->quantity;
    take_from_level(*entry, quantity, gone);
    if (gone) {
        orders_.erase(ref);
    } else {
        entry->quantity -= quantity;
    }
}

void BookBuilder::on_message(const AddOrder& m) {
    ++stats_.adds;
    insert(m.order_ref, Entry{m.header.locate, m.side, static_cast<Price>(m.price),
                              static_cast<Quantity>(m.shares)});
}

void BookBuilder::on_message(const OrderExecuted& m) {
    ++stats_.executions;
    reduce(m.order_ref, static_cast<Quantity>(m.executed_shares), true);
}

void BookBuilder::on_message(const OrderCancel& m) {
    ++stats_.cancels;
    reduce(m.order_ref, static_cast<Quantity>(m.cancelled_shares), false);
}

void BookBuilder::on_message(const OrderDelete& m) {
    ++stats_.deletes;
    const Entry* entry = orders_.find(m.order_ref);
    if (entry == nullptr) {
        ++stats_.anomalies;
        return;
    }
    take_from_level(*entry, entry->quantity, true);
    orders_.erase(m.order_ref);
}

void BookBuilder::on_message(const OrderReplace& m) {
    ++stats_.replaces;
    const Entry* entry = orders_.find(m.original_ref);
    if (entry == nullptr) {
        ++stats_.anomalies;
        return;
    }
    const Entry replacement{entry->locate, entry->side, static_cast<Price>(m.price),
                            static_cast<Quantity>(m.shares)};
    take_from_level(*entry, entry->quantity, true);
    orders_.erase(m.original_ref);
    insert(m.new_ref, replacement);
}

std::vector<LevelSnapshot> BookBuilder::depth(std::uint16_t locate, Side side,
                                              std::size_t max_levels) const {
    std::vector<LevelSnapshot> out;
    const InstrumentState* s = instrument(locate);
    if (s == nullptr) {
        return out;
    }
    const auto collect = [&](const auto& levels) {
        for (const auto& [price, level] : levels) {
            if (out.size() == max_levels) {
                break;
            }
            out.push_back(LevelSnapshot{price, level.quantity, level.quantity, level.orders});
        }
    };
    if (side == Side::Buy) {
        collect(s->bids);
    } else {
        collect(s->asks);
    }
    return out;
}

std::optional<Price> BookBuilder::best_bid(std::uint16_t locate) const {
    const InstrumentState* s = instrument(locate);
    if (s == nullptr || s->bids.empty()) {
        return std::nullopt;
    }
    return s->bids.begin()->first;
}

std::optional<Price> BookBuilder::best_ask(std::uint16_t locate) const {
    const InstrumentState* s = instrument(locate);
    if (s == nullptr || s->asks.empty()) {
        return std::nullopt;
    }
    return s->asks.begin()->first;
}

}  // namespace quill::itch
