#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "quill/flat_hash_map.hpp"
#include "quill/itch/messages.hpp"
#include "quill/order_book.hpp"
#include "quill/types.hpp"

namespace quill::itch {

/// Rebuilds full-depth, order-level books from an ITCH feed.
///
/// This is the consumer side of the feed: it only knows what the wire says, so it tracks
/// displayed quantity, not iceberg reserve. Messages that reference unknown orders or
/// would drive quantities negative are counted as anomalies rather than trusted, which is
/// what a production feed handler would do when it joins a feed mid-session.
class BookBuilder {
  public:
    struct Level {
        Quantity quantity = 0;
        std::size_t orders = 0;
    };

    struct InstrumentState {
        std::string symbol;
        std::uint32_t round_lot = 0;
        std::uint32_t tick_size = 0;
        std::map<Price, Level, std::greater<>> bids;
        std::map<Price, Level> asks;
        std::uint64_t executions = 0;
        Quantity executed_volume = 0;
        std::optional<Price> last_price;
    };

    struct Stats {
        std::uint64_t system_events = 0;
        std::uint64_t directory = 0;
        std::uint64_t adds = 0;
        std::uint64_t executions = 0;
        std::uint64_t cancels = 0;
        std::uint64_t deletes = 0;
        std::uint64_t replaces = 0;
        /// References to unknown orders, duplicate adds, or over-executions.
        std::uint64_t anomalies = 0;
    };

    void on_message(const SystemEvent& m);
    void on_message(const StockDirectory& m);
    void on_message(const AddOrder& m);
    void on_message(const OrderExecuted& m);
    void on_message(const OrderCancel& m);
    void on_message(const OrderDelete& m);
    void on_message(const OrderReplace& m);

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t live_orders() const noexcept { return orders_.size(); }
    [[nodiscard]] const std::vector<std::uint16_t>& locates() const noexcept { return locates_; }
    [[nodiscard]] const InstrumentState* instrument(std::uint16_t locate) const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> find(const std::string& symbol) const;

    /// Displayed depth, best first, in the same shape the matching engine reports.
    [[nodiscard]] std::vector<LevelSnapshot> depth(std::uint16_t locate, Side side,
                                                   std::size_t max_levels) const;
    [[nodiscard]] std::optional<Price> best_bid(std::uint16_t locate) const;
    [[nodiscard]] std::optional<Price> best_ask(std::uint16_t locate) const;

    [[nodiscard]] bool end_of_messages() const noexcept { return ended_; }

  private:
    struct Entry {
        std::uint16_t locate = 0;
        Side side = Side::Buy;
        Price price = 0;
        Quantity quantity = 0;
    };

    InstrumentState& state(std::uint16_t locate);
    void add_to_level(const Entry& entry);
    /// Removes `quantity` from the entry's level; `remove_order` also drops the order
    /// count and erases the level when it empties.
    void take_from_level(const Entry& entry, Quantity quantity, bool remove_order);
    void insert(std::uint64_t ref, const Entry& entry);
    void reduce(std::uint64_t ref, Quantity quantity, bool executed);

    std::vector<InstrumentState> instruments_;  // indexed by locate
    std::vector<std::uint16_t> locates_;
    FlatHashMap<std::uint64_t, Entry> orders_{1 << 16};
    Stats stats_;
    bool ended_ = false;
};

}  // namespace quill::itch
