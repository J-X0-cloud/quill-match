#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "quill/itch/messages.hpp"

namespace quill::itch {

// ---------------------------------------------------------------------------------------
// Big-endian primitives
// ---------------------------------------------------------------------------------------

template <std::size_t Bytes>
[[nodiscard]] inline std::uint64_t load_be(const std::uint8_t* p) noexcept {
    static_assert(Bytes >= 1 && Bytes <= 8);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < Bytes; ++i) {
        value = (value << 8U) | p[i];
    }
    return value;
}

template <std::size_t Bytes>
inline void store_be(std::uint8_t* p, std::uint64_t value) noexcept {
    static_assert(Bytes >= 1 && Bytes <= 8);
    for (std::size_t i = Bytes; i > 0; --i) {
        p[i - 1] = static_cast<std::uint8_t>(value & 0xFFU);
        value >>= 8U;
    }
}

// ---------------------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------------------

/// Appends length-prefixed ITCH frames to an in-memory buffer.
class Writer {
  public:
    void write(const SystemEvent& m);
    void write(const StockDirectory& m);
    void write(const AddOrder& m);
    void write(const OrderExecuted& m);
    void write(const OrderCancel& m);
    void write(const OrderDelete& m);
    void write(const OrderReplace& m);

    [[nodiscard]] std::span<const std::uint8_t> data() const noexcept { return buffer_; }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }
    void clear() noexcept { buffer_.clear(); }

  private:
    /// Reserves a frame for a message of `size` bytes, writes the length prefix and the
    /// common header, and returns a pointer to the first body byte.
    std::uint8_t* begin_frame(char type, std::size_t size, const Header& header);

    std::vector<std::uint8_t> buffer_;
    std::uint64_t messages_ = 0;
};

// ---------------------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------------------

enum class DecodeError : std::uint8_t {
    /// A known message type arrived with the wrong length.
    BadLength,
    /// A zero-length frame, which cannot carry a message type.
    EmptyFrame,
    /// A field held a value outside its domain (for example a side other than B or S).
    BadField,
};

[[nodiscard]] std::string_view to_string(DecodeError error) noexcept;

struct DecodeResult {
    /// Bytes fully consumed. Anything after this offset is either an incomplete trailing
    /// frame (when `error` is empty) or the frame that failed to decode.
    std::size_t consumed = 0;
    std::uint64_t messages = 0;
    /// Well-framed messages of types this decoder does not understand; they are skipped.
    std::uint64_t skipped = 0;
    std::optional<DecodeError> error;
};

namespace detail {

[[nodiscard]] inline Header read_header(const std::uint8_t* p) noexcept {
    // p[0] is the type byte.
    return Header{static_cast<std::uint16_t>(load_be<2>(p + 1)),
                  static_cast<std::uint16_t>(load_be<2>(p + 3)), load_be<6>(p + 5)};
}

[[nodiscard]] inline Symbol read_symbol(const std::uint8_t* p) noexcept {
    Symbol s{};
    std::memcpy(s.data(), p, s.size());
    return s;
}

[[nodiscard]] inline std::optional<Side> read_side(std::uint8_t byte) noexcept {
    if (byte == 'B') {
        return Side::Buy;
    }
    if (byte == 'S') {
        return Side::Sell;
    }
    return std::nullopt;
}

}  // namespace detail

/// Decodes one message body (type byte included) of exactly `size` bytes and passes it
/// to `handler.on_message(...)`. Returns false with `error` set if the body is malformed,
/// and true with `known == false` if the type is not one this decoder understands.
template <typename Handler>
bool decode_message(const std::uint8_t* p, std::size_t size, Handler& handler, bool& known,
                    DecodeError& error) {
    using detail::read_header;
    known = true;
    const auto expect = [&](std::size_t wanted) {
        if (size != wanted) {
            error = DecodeError::BadLength;
            return false;
        }
        return true;
    };
    switch (static_cast<char>(p[0])) {
        case msg_type::kSystemEvent: {
            if (!expect(SystemEvent::kSize)) {
                return false;
            }
            handler.on_message(SystemEvent{read_header(p), static_cast<char>(p[11])});
            return true;
        }
        case msg_type::kStockDirectory: {
            if (!expect(StockDirectory::kSize)) {
                return false;
            }
            handler.on_message(StockDirectory{read_header(p), detail::read_symbol(p + 11),
                                              static_cast<std::uint32_t>(load_be<4>(p + 19)),
                                              static_cast<std::uint32_t>(load_be<4>(p + 23))});
            return true;
        }
        case msg_type::kAddOrder: {
            if (!expect(AddOrder::kSize)) {
                return false;
            }
            const auto side = detail::read_side(p[19]);
            if (!side) {
                error = DecodeError::BadField;
                return false;
            }
            handler.on_message(AddOrder{read_header(p), load_be<8>(p + 11), *side,
                                        static_cast<std::uint32_t>(load_be<4>(p + 20)),
                                        detail::read_symbol(p + 24),
                                        static_cast<std::uint32_t>(load_be<4>(p + 32))});
            return true;
        }
        case msg_type::kOrderExecuted: {
            if (!expect(OrderExecuted::kSize)) {
                return false;
            }
            handler.on_message(OrderExecuted{read_header(p), load_be<8>(p + 11),
                                             static_cast<std::uint32_t>(load_be<4>(p + 19)),
                                             load_be<8>(p + 23)});
            return true;
        }
        case msg_type::kOrderCancel: {
            if (!expect(OrderCancel::kSize)) {
                return false;
            }
            handler.on_message(OrderCancel{read_header(p), load_be<8>(p + 11),
                                           static_cast<std::uint32_t>(load_be<4>(p + 19))});
            return true;
        }
        case msg_type::kOrderDelete: {
            if (!expect(OrderDelete::kSize)) {
                return false;
            }
            handler.on_message(OrderDelete{read_header(p), load_be<8>(p + 11)});
            return true;
        }
        case msg_type::kOrderReplace: {
            if (!expect(OrderReplace::kSize)) {
                return false;
            }
            handler.on_message(OrderReplace{read_header(p), load_be<8>(p + 11), load_be<8>(p + 19),
                                            static_cast<std::uint32_t>(load_be<4>(p + 27)),
                                            static_cast<std::uint32_t>(load_be<4>(p + 31))});
            return true;
        }
        default:
            known = false;
            return true;
    }
}

/// Decodes as many complete frames from `data` as possible.
///
/// Designed for streaming: a frame cut off at the end of the buffer is not an error, the
/// caller simply resumes from `consumed` once more bytes are available.
template <typename Handler>
DecodeResult decode(std::span<const std::uint8_t> data, Handler& handler) {
    DecodeResult result;
    std::size_t offset = 0;
    while (data.size() - offset >= 2) {
        const auto length = static_cast<std::size_t>(load_be<2>(data.data() + offset));
        if (length == 0) {
            result.error = DecodeError::EmptyFrame;
            break;
        }
        if (data.size() - offset - 2 < length) {
            break;  // incomplete trailing frame
        }
        bool known = false;
        DecodeError error{};
        if (!decode_message(data.data() + offset + 2, length, handler, known, error)) {
            result.error = error;
            break;
        }
        if (known) {
            ++result.messages;
        } else {
            ++result.skipped;
        }
        offset += 2 + length;
    }
    result.consumed = offset;
    return result;
}

}  // namespace quill::itch
