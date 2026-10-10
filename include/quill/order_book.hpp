#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "quill/commands.hpp"
#include "quill/events.hpp"
#include "quill/flat_hash_map.hpp"
#include "quill/instrument.hpp"
#include "quill/object_pool.hpp"
#include "quill/order.hpp"
#include "quill/price_ladder.hpp"
#include "quill/types.hpp"

namespace quill {

/// Aggregated view of one price level.
struct LevelSnapshot {
    Price price = 0;
    Quantity visible = 0;
    Quantity total = 0;
    std::size_t orders = 0;

    friend bool operator==(const LevelSnapshot&, const LevelSnapshot&) = default;
};

/// Central limit order book for a single instrument with price-time priority.
///
/// The book owns its orders and levels and reports everything that happens through the
/// supplied listener. It is not thread-safe: exactly one thread (the matching thread)
/// may call into it.
class OrderBook {
  public:
    OrderBook(InstrumentId id, InstrumentSpec spec, EventListener& listener,
              MatchId& match_sequence);

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = delete;
    ~OrderBook() = default;

    /// Validates and executes a new order under the id the engine chose for it. Returns
    /// true if the order was accepted (whether or not anything traded or rested).
    bool submit(const NewOrderRequest& request, OrderId order_id, Timestamp now);
    void cancel(const CancelRequest& request, Timestamp now);
    void replace(const ReplaceRequest& request, Timestamp now);

    [[nodiscard]] InstrumentId id() const noexcept { return id_; }
    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<Price> last_trade_price() const noexcept { return last_trade_; }

    [[nodiscard]] const Order* find(OrderId order_id) const noexcept;
    [[nodiscard]] bool contains(OrderId order_id) const noexcept {
        return index_.contains(order_id);
    }
    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] const PriceLadder& ladder(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    /// Up to `max_levels` levels on `side`, best first.
    [[nodiscard]] std::vector<LevelSnapshot> depth(Side side, std::size_t max_levels) const;

    /// Visits resting orders on `side` in priority order (best price, then time).
    template <typename Fn>
    void for_each_order(Side side, Fn&& fn) const {
        const auto levels = ladder(side).levels();
        for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
            for (const Order* order = (*it)->front(); order != nullptr; order = order->next) {
                fn(*order);
            }
        }
    }

    /// Exhaustively checks internal consistency. Intended for tests and debug tooling;
    /// returns a description of the first violation found, or nullopt if consistent.
    [[nodiscard]] std::optional<std::string> check_invariants() const;

  private:
    [[nodiscard]] RejectReason validate_new(const NewOrderRequest& request) const noexcept;
    [[nodiscard]] RejectReason validate_price(Price price) const noexcept;
    [[nodiscard]] RejectReason validate_quantity(Quantity quantity) const noexcept;
    /// Worst price a market order may reach given the collar, if one is in force.
    [[nodiscard]] std::optional<Price> market_limit(Side side) const noexcept;
    [[nodiscard]] bool would_cross(Side side, Price price) const noexcept;
    [[nodiscard]] bool fill_or_kill_fillable(const NewOrderRequest& request,
                                             std::optional<Price> limit) const noexcept;

    /// Outcome of running an order against the opposite side.
    enum class MatchResult { Done, StoppedBySelfTrade };

    MatchResult match(Order& aggressor, std::optional<Price> limit, Timestamp now);
    void fill(Order& aggressor, Order& resting, Timestamp now);
    void rest(Order& order, Timestamp now);
    /// Unlinks a resting order from its level, dropping the level when it empties.
    void unlink(Order& order) noexcept;
    void cancel_resting(Order& order, std::uint64_t client_tag, CancelReason reason, Timestamp now);
    void cancel_remainder(Order& order, CancelReason reason, Timestamp now);
    void release(Order& order) noexcept;

    void reject(RequestKind kind, OrderId order_id, std::uint64_t client_tag, RejectReason reason,
                Timestamp now);

    PriceLadder& ladder_for(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }

    InstrumentId id_;
    InstrumentSpec spec_;
    EventListener* listener_;
    MatchId* match_sequence_;
    PriceLadder bids_{Side::Buy};
    PriceLadder asks_{Side::Sell};
    ObjectPool<Order> orders_{1024};
    FlatHashMap<OrderId, Order*> index_{1024};
    std::optional<Price> last_trade_;
};

}  // namespace quill
