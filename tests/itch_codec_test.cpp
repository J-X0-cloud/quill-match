#include <gtest/gtest.h>

#include <variant>
#include <vector>

#include "quill/itch/codec.hpp"

namespace quill::itch {
namespace {

using AnyMessage = std::variant<SystemEvent, StockDirectory, AddOrder, OrderExecuted, OrderCancel,
                                OrderDelete, OrderReplace>;

struct Collector {
    std::vector<AnyMessage> messages;
    template <typename M>
    void on_message(const M& m) {
        messages.emplace_back(m);
    }
};

std::vector<AnyMessage> sample_messages() {
    const Header h{7, 3, 34'200'000'000'123ULL};
    return {
        SystemEvent{h, system_event::kStartOfMessages},
        StockDirectory{h, make_symbol("AAPL"), 100, 100},
        AddOrder{h, 0x0102030405060708ULL, Side::Sell, 250, make_symbol("AAPL"), 1'895'000},
        OrderExecuted{h, 42, 50, 999},
        OrderCancel{h, 42, 25},
        OrderDelete{h, 42},
        OrderReplace{h, 42, 43, 100, 1'896'000},
    };
}

Writer encode_all(const std::vector<AnyMessage>& messages) {
    Writer writer;
    for (const auto& m : messages) {
        std::visit([&](const auto& concrete) { writer.write(concrete); }, m);
    }
    return writer;
}

TEST(ItchCodec, BigEndianHelpers) {
    std::uint8_t buf[8] = {};
    store_be<6>(buf, 0x0000AABBCCDDEEFFULL);
    EXPECT_EQ(buf[0], 0xAA);
    EXPECT_EQ(buf[5], 0xFF);
    EXPECT_EQ(load_be<6>(buf), 0xAABBCCDDEEFFULL);
    store_be<2>(buf, 0x1234);
    EXPECT_EQ(load_be<2>(buf), 0x1234U);
}

TEST(ItchCodec, SymbolPaddingRoundTrip) {
    const Symbol s = make_symbol("MSFT");
    EXPECT_EQ(std::string(s.begin(), s.end()), "MSFT    ");
    EXPECT_EQ(symbol_text(s), "MSFT");
    EXPECT_EQ(symbol_text(make_symbol("TOOLONGSYMBOL")), "TOOLONGS");
    EXPECT_EQ(symbol_text(make_symbol("")), "");
}

TEST(ItchCodec, AddOrderWireLayoutMatchesItch50) {
    Writer writer;
    writer.write(AddOrder{Header{1, 2, 0x010203040506ULL}, 0x1122334455667788ULL, Side::Buy,
                          0x0A0B0C0D, make_symbol("ABC"), 0x00989680});
    const std::vector<std::uint8_t> expected = {
        0x00, 0x24,                                      // frame length 36
        'A',                                             // type
        0x00, 0x01,                                      // locate
        0x00, 0x02,                                      // tracking
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,              // timestamp
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,  // order ref
        'B',                                             // side
        0x0A, 0x0B, 0x0C, 0x0D,                          // shares
        'A',  'B',  'C',  ' ',  ' ',  ' ',  ' ',  ' ',   // stock
        0x00, 0x98, 0x96, 0x80,                          // price 1000.0000
    };
    const auto data = writer.data();
    EXPECT_EQ(std::vector<std::uint8_t>(data.begin(), data.end()), expected);
}

TEST(ItchCodec, MessageSizesMatchSpecification) {
    EXPECT_EQ(AddOrder::kSize, 36U);
    EXPECT_EQ(OrderExecuted::kSize, 31U);
    EXPECT_EQ(OrderCancel::kSize, 23U);
    EXPECT_EQ(OrderDelete::kSize, 19U);
    EXPECT_EQ(OrderReplace::kSize, 35U);
}

TEST(ItchCodec, RoundTripsEveryMessageType) {
    const auto messages = sample_messages();
    const Writer writer = encode_all(messages);
    EXPECT_EQ(writer.messages(), messages.size());

    Collector collector;
    const DecodeResult result = decode(writer.data(), collector);
    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.consumed, writer.size());
    EXPECT_EQ(result.messages, messages.size());
    EXPECT_EQ(collector.messages, messages);
}

TEST(ItchCodec, TimestampWrapsAtMidnight) {
    Writer writer;
    writer.write(OrderDelete{Header{0, 0, kNanosPerDay + 5}, 1});
    Collector collector;
    decode(writer.data(), collector);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_EQ(std::get<OrderDelete>(collector.messages[0]).header.timestamp, 5U);
}

// Feeding the stream in two pieces split at every possible byte offset must always yield
// the same messages: this is what a network or file reader does with partial reads.
TEST(ItchCodec, StreamingDecodeAcrossArbitrarySplits) {
    const auto messages = sample_messages();
    const Writer writer = encode_all(messages);
    const auto bytes = writer.data();
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        Collector collector;
        std::vector<std::uint8_t> pending(bytes.begin(), bytes.begin() + static_cast<long>(split));
        DecodeResult first = decode(pending, collector);
        ASSERT_FALSE(first.error.has_value());
        pending.erase(pending.begin(), pending.begin() + static_cast<long>(first.consumed));
        pending.insert(pending.end(), bytes.begin() + static_cast<long>(split), bytes.end());
        DecodeResult second = decode(pending, collector);
        ASSERT_FALSE(second.error.has_value());
        EXPECT_EQ(second.consumed, pending.size());
        ASSERT_EQ(collector.messages, messages) << "split at " << split;
    }
}

TEST(ItchCodec, UnknownTypesAreSkipped) {
    std::vector<std::uint8_t> bytes = {0x00, 0x03, 'Z', 0x01, 0x02};
    Writer writer;
    writer.write(OrderDelete{Header{}, 9});
    bytes.insert(bytes.end(), writer.data().begin(), writer.data().end());
    Collector collector;
    const DecodeResult result = decode(bytes, collector);
    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.skipped, 1U);
    EXPECT_EQ(result.messages, 1U);
    EXPECT_EQ(result.consumed, bytes.size());
}

TEST(ItchCodec, WrongLengthForKnownTypeIsAnError) {
    const std::vector<std::uint8_t> bytes = {0x00, 0x04, 'D', 0x00, 0x00, 0x00};
    Collector collector;
    const DecodeResult result = decode(bytes, collector);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, DecodeError::BadLength);
    EXPECT_EQ(result.consumed, 0U);
    EXPECT_TRUE(collector.messages.empty());
}

TEST(ItchCodec, EmptyFrameIsAnError) {
    const std::vector<std::uint8_t> bytes = {0x00, 0x00};
    Collector collector;
    const DecodeResult result = decode(bytes, collector);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, DecodeError::EmptyFrame);
}

TEST(ItchCodec, InvalidSideIsAnError) {
    Writer writer;
    writer.write(AddOrder{Header{}, 1, Side::Buy, 1, make_symbol("X"), 1});
    std::vector<std::uint8_t> bytes(writer.data().begin(), writer.data().end());
    bytes[2 + 19] = 'Q';
    Collector collector;
    const DecodeResult result = decode(bytes, collector);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, DecodeError::BadField);
    EXPECT_EQ(to_string(*result.error), "field value out of range");
}

TEST(ItchCodec, ErrorStopsAfterGoodPrefix) {
    Writer writer;
    writer.write(OrderDelete{Header{}, 1});
    std::vector<std::uint8_t> bytes(writer.data().begin(), writer.data().end());
    const std::size_t good = bytes.size();
    bytes.insert(bytes.end(), {0x00, 0x00});
    Collector collector;
    const DecodeResult result = decode(bytes, collector);
    EXPECT_EQ(result.messages, 1U);
    EXPECT_EQ(result.consumed, good);
    EXPECT_TRUE(result.error.has_value());
}

}  // namespace
}  // namespace quill::itch
