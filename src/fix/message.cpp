#include "quill/fix/message.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>

namespace quill::fix {

std::string_view to_string(ParseError error) noexcept {
    switch (error) {
        case ParseError::Empty:
            return "empty message";
        case ParseError::MalformedField:
            return "malformed tag=value field";
        case ParseError::BadTagNumber:
            return "tag is not a positive integer";
        case ParseError::MissingBeginString:
            return "BeginString (8) must be the first field";
        case ParseError::MissingBodyLength:
            return "BodyLength (9) must be the second field";
        case ParseError::MissingMsgType:
            return "MsgType (35) must follow BodyLength";
        case ParseError::BodyLengthMismatch:
            return "BodyLength does not match the message";
        case ParseError::MissingCheckSum:
            return "CheckSum (10) must be the last field";
        case ParseError::CheckSumMismatch:
            return "CheckSum does not match the message";
        case ParseError::FieldsAfterCheckSum:
            return "fields after CheckSum (10)";
    }
    return "?";
}

unsigned checksum(std::string_view bytes) noexcept {
    unsigned sum = 0;
    for (const char c : bytes) {
        sum += static_cast<unsigned char>(c);
    }
    return sum % 256;
}

namespace {

std::optional<std::int64_t> to_int(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------------------

std::vector<Field> Message::fields() const {
    std::vector<Field> out;
    out.reserve(slots_.size());
    for (const Slot& slot : slots_) {
        out.push_back(Field{slot.tag, value(slot)});
    }
    return out;
}

std::string_view Message::msg_type() const noexcept {
    const auto type = get(tag::kMsgType);
    return type ? *type : std::string_view{};
}

std::optional<std::string_view> Message::get(int wanted) const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.tag == wanted) {
            return value(slot);
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> Message::get_int(int wanted) const noexcept {
    const auto value = get(wanted);
    return value ? to_int(*value) : std::nullopt;
}

Message::Result Message::parse(std::string_view raw, char delimiter, bool strict) {
    Result result;
    const auto fail = [&result](ParseError error, int tag = 0) {
        result.error = error;
        result.error_tag = tag;
        return result;
    };
    // Tolerate line endings left over from line-oriented input.
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) {
        raw.remove_suffix(1);
    }
    if (raw.empty()) {
        return fail(ParseError::Empty);
    }

    Message message;
    message.raw_.assign(raw);
    const std::string_view text(message.raw_);
    auto& slots = message.slots_;
    slots.reserve(32);

    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find(delimiter, pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view segment = text.substr(pos, end - pos);
        const std::size_t eq = segment.find('=');
        if (segment.empty() || eq == std::string_view::npos || eq == 0 ||
            eq + 1 == segment.size()) {
            return fail(ParseError::MalformedField);
        }
        const auto tag_number = to_int(segment.substr(0, eq));
        if (!tag_number || *tag_number <= 0 || *tag_number > 99'999) {
            return fail(ParseError::BadTagNumber);
        }
        slots.push_back(Slot{static_cast<int>(*tag_number), static_cast<std::uint32_t>(pos),
                             static_cast<std::uint32_t>(pos + eq + 1),
                             static_cast<std::uint32_t>(segment.size() - eq - 1)});
        pos = end + 1;
    }

    if (slots.empty() || slots[0].tag != tag::kBeginString) {
        return fail(ParseError::MissingBeginString, tag::kBeginString);
    }
    std::size_t next = 1;
    const bool has_body_length = slots.size() > 1 && slots[1].tag == tag::kBodyLength;
    if (has_body_length) {
        next = 2;
    } else if (strict) {
        return fail(ParseError::MissingBodyLength, tag::kBodyLength);
    }
    if (slots.size() <= next || slots[next].tag != tag::kMsgType) {
        return fail(ParseError::MissingMsgType, tag::kMsgType);
    }

    const auto checksum_it = std::find_if(slots.begin(), slots.end(),
                                          [](const Slot& f) { return f.tag == tag::kCheckSum; });
    if (checksum_it != slots.end() && checksum_it + 1 != slots.end()) {
        return fail(ParseError::FieldsAfterCheckSum, tag::kCheckSum);
    }
    if (checksum_it == slots.end()) {
        if (strict) {
            return fail(ParseError::MissingCheckSum, tag::kCheckSum);
        }
    } else if (strict) {
        const std::size_t checksum_start = checksum_it->start;
        const std::string_view declared_text = message.value(*checksum_it);
        if (declared_text.size() != 3) {
            return fail(ParseError::CheckSumMismatch, tag::kCheckSum);
        }
        const auto declared = to_int(declared_text);
        if (!declared ||
            static_cast<unsigned>(*declared) != checksum(text.substr(0, checksum_start))) {
            return fail(ParseError::CheckSumMismatch, tag::kCheckSum);
        }
        // BodyLength counts from the first byte after the 9= field's delimiter up to and
        // including the delimiter that precedes 10=.
        const auto declared_length = to_int(message.value(slots[1]));
        const std::size_t body_start = slots[2].start;
        if (!declared_length ||
            static_cast<std::size_t>(*declared_length) != checksum_start - body_start) {
            return fail(ParseError::BodyLengthMismatch, tag::kBodyLength);
        }
    }
    result.message = std::move(message);
    return result;
}

// ---------------------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------------------

Builder::Builder(std::string_view msg_type, char delimiter) : delimiter_(delimiter) {
    body_.reserve(256);
    add(tag::kMsgType, msg_type);
}

Builder& Builder::add(int tag_number, std::string_view value) {
    body_ += std::to_string(tag_number);
    body_ += '=';
    body_ += value;
    body_ += delimiter_;
    return *this;
}

Builder& Builder::add(int tag_number, std::int64_t value) {
    return add(tag_number, std::string_view(std::to_string(value)));
}

Builder& Builder::add(int tag_number, char value) {
    return add(tag_number, std::string_view(&value, 1));
}

std::string Builder::finish(std::string_view begin_string) const {
    std::string out;
    out.reserve(body_.size() + 32);
    out += "8=";
    out += begin_string;
    out += delimiter_;
    out += "9=";
    out += std::to_string(body_.size());
    out += delimiter_;
    out += body_;
    char trailer[8];
    std::snprintf(trailer, sizeof(trailer), "10=%03u", checksum(out));
    out += trailer;
    out += delimiter_;
    return out;
}

// ---------------------------------------------------------------------------------------
// Value formats
// ---------------------------------------------------------------------------------------

std::optional<Price> parse_price(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    std::string_view fraction =
        dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() && fraction.empty()) {
        return std::nullopt;
    }
    const auto all_digits = [](std::string_view s) {
        return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!all_digits(whole) || !all_digits(fraction) || whole.size() > 12) {
        return std::nullopt;
    }
    while (fraction.size() > 4 && fraction.back() == '0') {
        fraction.remove_suffix(1);
    }
    if (fraction.size() > 4) {
        return std::nullopt;  // finer than the engine's price resolution
    }
    Price value = 0;
    for (const char c : whole) {
        value = value * 10 + (c - '0');
    }
    Price frac = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        frac = frac * 10 + (i < fraction.size() ? fraction[i] - '0' : 0);
    }
    return value * kPriceScale + frac;
}

std::string format_price(Price price) {
    std::string out;
    if (price < 0) {
        out += '-';
        price = -price;
    }
    out += std::to_string(price / kPriceScale);
    char frac[8];
    std::snprintf(frac, sizeof(frac), "%04lld", static_cast<long long>(price % kPriceScale));
    std::string_view digits(frac, 4);
    while (digits.size() > 2 && digits.back() == '0') {
        digits.remove_suffix(1);
    }
    out += '.';
    out += digits;
    return out;
}

std::string format_decimal(double value, int max_decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", max_decimals, value);
    std::string out(buffer);
    if (out.find('.') != std::string::npos) {
        while (out.back() == '0') {
            out.pop_back();
        }
        if (out.back() == '.') {
            out.pop_back();
        }
    }
    return out;
}

std::string format_utc_timestamp(Timestamp nanos_since_epoch) {
    using namespace std::chrono;
    const sys_time<nanoseconds> tp{nanoseconds{static_cast<std::int64_t>(nanos_since_epoch)}};
    const auto day = floor<days>(tp);
    const year_month_day ymd{day};
    const hh_mm_ss<milliseconds> tod{duration_cast<milliseconds>(tp - day)};
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d%02u%02u-%02lld:%02lld:%02lld.%03lld",
                  static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()), static_cast<long long>(tod.hours().count()),
                  static_cast<long long>(tod.minutes().count()),
                  static_cast<long long>(tod.seconds().count()),
                  static_cast<long long>(tod.subseconds().count()));
    return buffer;
}

}  // namespace quill::fix
