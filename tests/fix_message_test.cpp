#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "quill/fix/message.hpp"

namespace quill::fix {
namespace {

std::string pipes(std::string text) {
    for (char& c : text) {
        if (c == kSoh) {
            c = '|';
        }
    }
    return text;
}

TEST(FixMessage, BuilderProducesValidEnvelope) {
    Builder builder(msg::kHeartbeat);
    builder.add(tag::kSenderCompId, std::string_view("A"))
        .add(tag::kTargetCompId, std::string_view("B"))
        .add(tag::kMsgSeqNum, std::int64_t{1});
    const std::string text = builder.finish();
    // Body is "35=0|49=A|56=B|34=1|" = 20 bytes.
    EXPECT_EQ(pipes(text).substr(0, 25), "8=FIX.4.4|9=20|35=0|49=A|");
    const auto parsed = Message::parse(text);
    ASSERT_TRUE(parsed.message.has_value()) << to_string(*parsed.error);
    EXPECT_EQ(parsed.message->msg_type(), "0");
    EXPECT_EQ(parsed.message->get(tag::kSenderCompId), "A");
    EXPECT_EQ(parsed.message->get_int(tag::kMsgSeqNum), 1);
}

TEST(FixMessage, ChecksumIsByteSumModulo256) {
    EXPECT_EQ(checksum(""), 0U);
    EXPECT_EQ(checksum("A"), 65U);
    EXPECT_EQ(checksum(std::string(4, '\xFF')), (4U * 255U) % 256U);
}

TEST(FixMessage, KnownGoodMessageParses) {
    // A classic example with a hand-computed body length and checksum.
    const std::string body = "35=D|49=C|56=QUILL|34=2|11=X1|55=AAA|54=1|38=10|40=2|44=1.5|";
    const std::string header = "8=FIX.4.4|9=" + std::to_string(body.size()) + "|";
    const std::string prefix = header + body;
    // The checksum covers the bytes as transmitted, so with '|' as the delimiter it is
    // computed over the pipes themselves.
    char trailer[16];
    std::snprintf(trailer, sizeof(trailer), "10=%03u|", checksum(prefix));
    const auto parsed = Message::parse(prefix + trailer, '|');
    ASSERT_TRUE(parsed.message.has_value()) << to_string(*parsed.error);
    EXPECT_EQ(parsed.message->get(tag::kPrice), "1.5");
    EXPECT_EQ(parsed.message->field_count(), 13U);
}

TEST(FixMessage, DetectsCorruption) {
    Builder builder(msg::kHeartbeat, '|');
    builder.add(tag::kSenderCompId, std::string_view("ABC"));
    std::string text = builder.finish();

    std::string bad_checksum = text;
    bad_checksum[bad_checksum.size() - 2] =
        bad_checksum[bad_checksum.size() - 2] == '0' ? '1' : '0';
    EXPECT_EQ(Message::parse(bad_checksum, '|').error, ParseError::CheckSumMismatch);

    std::string flipped = text;
    flipped[flipped.find("ABC")] = 'X';  // body changed, checksum no longer matches
    EXPECT_EQ(Message::parse(flipped, '|').error, ParseError::CheckSumMismatch);

    std::string bad_length = text;
    bad_length.replace(bad_length.find("9=") + 2, 2, "99");
    const auto parsed = Message::parse(bad_length, '|');
    ASSERT_TRUE(parsed.error.has_value());
    EXPECT_TRUE(*parsed.error == ParseError::BodyLengthMismatch ||
                *parsed.error == ParseError::CheckSumMismatch);
}

TEST(FixMessage, EnforcesFieldOrder) {
    EXPECT_EQ(Message::parse("35=0|8=FIX.4.4|", '|', false).error, ParseError::MissingBeginString);
    EXPECT_EQ(Message::parse("8=FIX.4.4|35=0|", '|', true).error, ParseError::MissingBodyLength);
    EXPECT_EQ(Message::parse("8=FIX.4.4|49=X|35=0|", '|', false).error, ParseError::MissingMsgType);
    EXPECT_EQ(Message::parse("8=FIX.4.4|35=0|10=000|49=X|", '|', false).error,
              ParseError::FieldsAfterCheckSum);
    EXPECT_EQ(Message::parse("8=FIX.4.4|9=5|35=0|", '|', true).error, ParseError::MissingCheckSum);
}

TEST(FixMessage, RejectsMalformedFields) {
    EXPECT_EQ(Message::parse("", '|').error, ParseError::Empty);
    EXPECT_EQ(Message::parse("8=FIX.4.4||35=0|", '|', false).error, ParseError::MalformedField);
    EXPECT_EQ(Message::parse("8=FIX.4.4|35|", '|', false).error, ParseError::MalformedField);
    EXPECT_EQ(Message::parse("8=FIX.4.4|=0|", '|', false).error, ParseError::MalformedField);
    EXPECT_EQ(Message::parse("8=FIX.4.4|35=|", '|', false).error, ParseError::MalformedField);
    EXPECT_EQ(Message::parse("8=FIX.4.4|3x=0|", '|', false).error, ParseError::BadTagNumber);
    EXPECT_EQ(Message::parse("8=FIX.4.4|-1=0|", '|', false).error, ParseError::BadTagNumber);
}

TEST(FixMessage, LenientModeAcceptsHandWrittenInput) {
    const auto parsed = Message::parse("8=FIX.4.4|35=D|11=abc|38=100\r\n", '|', false);
    ASSERT_TRUE(parsed.message.has_value());
    EXPECT_EQ(parsed.message->get(tag::kClOrdId), "abc");
    EXPECT_EQ(parsed.message->get_int(tag::kOrderQty), 100);
    EXPECT_FALSE(parsed.message->get_int(tag::kClOrdId).has_value());
    EXPECT_FALSE(parsed.message->has(tag::kPrice));
}

TEST(FixMessage, CopiesAndMovesStayValid) {
    auto parsed = Message::parse("8=FIX.4.4|35=0|49=S|", '|', false);
    ASSERT_TRUE(parsed.message.has_value());
    Message copy = *parsed.message;
    Message moved = std::move(*parsed.message);
    EXPECT_EQ(copy.get(tag::kSenderCompId), "S");
    EXPECT_EQ(moved.get(tag::kSenderCompId), "S");
    const auto fields = moved.fields();
    ASSERT_EQ(fields.size(), 3U);
    EXPECT_EQ(fields[2].tag, 49);
}

TEST(FixValues, ParsePrice) {
    EXPECT_EQ(parse_price("123.45"), 1'234'500);
    EXPECT_EQ(parse_price("0.0001"), 1);
    EXPECT_EQ(parse_price("7"), 70'000);
    EXPECT_EQ(parse_price("7."), 70'000);
    EXPECT_EQ(parse_price(".5"), 5'000);
    EXPECT_EQ(parse_price("1.230000"), 12'300);
    EXPECT_FALSE(parse_price("").has_value());
    EXPECT_FALSE(parse_price(".").has_value());
    EXPECT_FALSE(parse_price("1.00001").has_value());
    EXPECT_FALSE(parse_price("-1").has_value());
    EXPECT_FALSE(parse_price("1e3").has_value());
    EXPECT_FALSE(parse_price("1.2.3").has_value());
    EXPECT_FALSE(parse_price(" 1").has_value());
    EXPECT_FALSE(parse_price("1234567890123").has_value());
}

TEST(FixValues, FormatPrice) {
    EXPECT_EQ(format_price(1'234'500), "123.45");
    EXPECT_EQ(format_price(1'234'567), "123.4567");
    EXPECT_EQ(format_price(10'000), "1.00");
    EXPECT_EQ(format_price(5), "0.0005");
    EXPECT_EQ(format_price(-15'000), "-1.50");
    for (Price p : {Price{1}, Price{99'999}, Price{123'456'789}}) {
        EXPECT_EQ(parse_price(format_price(p)), p);
    }
}

TEST(FixValues, FormatDecimal) {
    EXPECT_EQ(format_decimal(100.5), "100.5");
    EXPECT_EQ(format_decimal(100.0), "100");
    EXPECT_EQ(format_decimal(0.1234567), "0.123457");
}

TEST(FixValues, FormatUtcTimestamp) {
    EXPECT_EQ(format_utc_timestamp(0), "19700101-00:00:00.000");
    // 2026-03-04 05:06:07.089 UTC
    EXPECT_EQ(format_utc_timestamp(1'772'600'767'089'000'000ULL), "20260304-05:06:07.089");
}

}  // namespace
}  // namespace quill::fix
