#pragma once

#include <cstdint>
#include <ostream>
#include <vector>

#include "quill/events.hpp"
#include "quill/instrument.hpp"
#include "quill/itch/codec.hpp"
#include "quill/itch/messages.hpp"

namespace quill::itch {

/// Turns the engine's public market data events into an ITCH-style feed.
///
/// Attach it to the engine (directly or through an EventFanout), call
/// register_instrument() for each instrument so the feed carries a stock directory, and
/// drain the encoded bytes with flush() whenever convenient. Instrument ids are used as
/// stock locate codes.
class Publisher final : public EventListener {
  public:
    void start_of_messages(Timestamp now);
    void end_of_messages(Timestamp now);
    void register_instrument(InstrumentId id, const InstrumentSpec& spec, Timestamp now);

    void on_book_add(const BookOrderAdded& e) override;
    void on_book_execute(const BookOrderExecuted& e) override;
    void on_book_reduce(const BookOrderReduced& e) override;
    void on_book_delete(const BookOrderDeleted& e) override;

    /// Writes everything encoded so far to `out` and clears the internal buffer.
    void flush(std::ostream& out);

    [[nodiscard]] const Writer& writer() const noexcept { return writer_; }
    [[nodiscard]] std::uint64_t messages() const noexcept { return writer_.messages(); }

  private:
    [[nodiscard]] Header header(InstrumentId id, Timestamp t) noexcept;
    [[nodiscard]] const Symbol& symbol(InstrumentId id) const;

    Writer writer_;
    std::vector<Symbol> symbols_;  // indexed by instrument id
    std::uint16_t tracking_ = 0;
};

}  // namespace quill::itch
