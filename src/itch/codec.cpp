#include "quill/itch/codec.hpp"

#include <algorithm>
#include <limits>

namespace quill::itch {

Symbol make_symbol(std::string_view text) noexcept {
    Symbol symbol;
    symbol.fill(' ');
    std::copy_n(text.begin(), std::min(text.size(), symbol.size()), symbol.begin());
    return symbol;
}

std::string symbol_text(const Symbol& symbol) {
    std::string text(symbol.begin(), symbol.end());
    const auto end = text.find_last_not_of(' ');
    text.erase(end == std::string::npos ? 0 : end + 1);
    return text;
}

std::string_view to_string(DecodeError error) noexcept {
    switch (error) {
        case DecodeError::BadLength:
            return "message length does not match its type";
        case DecodeError::EmptyFrame:
            return "zero-length frame";
        case DecodeError::BadField:
            return "field value out of range";
    }
    return "?";
}

std::uint8_t* Writer::begin_frame(char type, std::size_t size, const Header& header) {
    static_assert(std::numeric_limits<std::uint16_t>::max() >= OrderReplace::kSize);
    const std::size_t start = buffer_.size();
    buffer_.resize(start + 2 + size);
    std::uint8_t* p = buffer_.data() + start;
    store_be<2>(p, size);
    p += 2;
    p[0] = static_cast<std::uint8_t>(type);
    store_be<2>(p + 1, header.locate);
    store_be<2>(p + 3, header.tracking);
    store_be<6>(p + 5, header.timestamp % kNanosPerDay);
    ++messages_;
    return p + kHeaderSize;
}

void Writer::write(const SystemEvent& m) {
    std::uint8_t* p = begin_frame(SystemEvent::kType, SystemEvent::kSize, m.header);
    p[0] = static_cast<std::uint8_t>(m.event_code);
}

void Writer::write(const StockDirectory& m) {
    std::uint8_t* p = begin_frame(StockDirectory::kType, StockDirectory::kSize, m.header);
    std::copy(m.stock.begin(), m.stock.end(), p);
    store_be<4>(p + 8, m.round_lot_size);
    store_be<4>(p + 12, m.tick_size);
}

void Writer::write(const AddOrder& m) {
    std::uint8_t* p = begin_frame(AddOrder::kType, AddOrder::kSize, m.header);
    store_be<8>(p, m.order_ref);
    p[8] = m.side == Side::Buy ? 'B' : 'S';
    store_be<4>(p + 9, m.shares);
    std::copy(m.stock.begin(), m.stock.end(), p + 13);
    store_be<4>(p + 21, m.price);
}

void Writer::write(const OrderExecuted& m) {
    std::uint8_t* p = begin_frame(OrderExecuted::kType, OrderExecuted::kSize, m.header);
    store_be<8>(p, m.order_ref);
    store_be<4>(p + 8, m.executed_shares);
    store_be<8>(p + 12, m.match_number);
}

void Writer::write(const OrderCancel& m) {
    std::uint8_t* p = begin_frame(OrderCancel::kType, OrderCancel::kSize, m.header);
    store_be<8>(p, m.order_ref);
    store_be<4>(p + 8, m.cancelled_shares);
}

void Writer::write(const OrderDelete& m) {
    std::uint8_t* p = begin_frame(OrderDelete::kType, OrderDelete::kSize, m.header);
    store_be<8>(p, m.order_ref);
}

void Writer::write(const OrderReplace& m) {
    std::uint8_t* p = begin_frame(OrderReplace::kType, OrderReplace::kSize, m.header);
    store_be<8>(p, m.original_ref);
    store_be<8>(p + 8, m.new_ref);
    store_be<4>(p + 16, m.shares);
    store_be<4>(p + 20, m.price);
}

}  // namespace quill::itch
