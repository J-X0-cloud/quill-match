#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "quill/clock.hpp"
#include "quill/commands.hpp"
#include "quill/events.hpp"
#include "quill/instrument.hpp"
#include "quill/order_book.hpp"

namespace quill {

/// Routes commands to per-instrument order books.
///
/// The engine owns the order id and match id sequences and stamps every event with the
/// time the command started processing. Instrument ids are dense and start at 1, which
/// lets them double as ITCH stock locate codes.
class MatchingEngine {
  public:
    MatchingEngine(EventListener& listener, Clock& clock);

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) = delete;
    MatchingEngine& operator=(MatchingEngine&&) = delete;
    ~MatchingEngine() = default;

    /// Registers an instrument and returns its id. Symbols must be unique.
    InstrumentId add_instrument(InstrumentSpec spec);

    [[nodiscard]] std::optional<InstrumentId> find_instrument(std::string_view symbol) const;
    [[nodiscard]] std::size_t instrument_count() const noexcept { return books_.size(); }
    [[nodiscard]] const OrderBook* book(InstrumentId id) const noexcept;

    /// Executes one command synchronously.
    void process(const Command& command);

    /// Returns the id of the accepted order, or kInvalidOrderId if it was rejected.
    OrderId submit(const NewOrderRequest& request);
    void cancel(const CancelRequest& request);
    void replace(const ReplaceRequest& request);

    [[nodiscard]] std::uint64_t commands_processed() const noexcept { return commands_; }
    [[nodiscard]] MatchId last_match_id() const noexcept { return match_sequence_; }

  private:
    OrderBook* lookup(InstrumentId id) noexcept;
    void reject_unknown_instrument(InstrumentId id, RequestKind kind, OrderId order_id,
                                   std::uint64_t client_tag);

    EventListener* listener_;
    Clock* clock_;
    // Books are heap-allocated so their addresses stay stable as instruments are added.
    std::vector<std::unique_ptr<OrderBook>> books_;
    OrderId next_order_id_ = 1;
    MatchId match_sequence_ = 0;
    std::uint64_t commands_ = 0;
};

}  // namespace quill
