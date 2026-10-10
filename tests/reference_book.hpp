#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "quill/commands.hpp"
#include "quill/types.hpp"

namespace quill::test {

/// Deliberately naive order book used as an oracle in randomized tests.
///
/// Every operation is a linear scan over a flat vector and priority is an explicit
/// sequence number, so the code reads like the rulebook rather than like an engine. It
/// shares no data structures or code paths with OrderBook. Validation is limited to the
/// rules the randomized generator can actually trigger.
class ReferenceBook {
  public:
    struct RestingOrder {
        OrderId id = 0;
        ParticipantId participant = 0;
        SelfTradePrevention stp = SelfTradePrevention::None;
        bool post_only = false;
        Side side = Side::Buy;
        Price price = 0;
        Quantity quantity = 0;
        Quantity executed = 0;
        Quantity visible = 0;
        Quantity hidden = 0;
        Quantity display = 0;
        std::uint64_t sequence = 0;

        friend bool operator==(const RestingOrder&, const RestingOrder&) = default;
    };

    struct Fill {
        OrderId aggressor = 0;
        OrderId resting = 0;
        Price price = 0;
        Quantity quantity = 0;

        friend bool operator==(const Fill&, const Fill&) = default;
    };

    std::vector<Fill> fills;

    bool submit(const NewOrderRequest& request, OrderId id) {
        const bool is_market = request.type == OrderType::Market;
        if (is_market && !has_side(opposite(request.side))) {
            return false;
        }
        if (request.post_only && crosses_opposite(request.side, request.price)) {
            return false;
        }
        Working order{};
        order.id = id;
        order.participant = request.participant;
        order.stp = request.stp;
        order.side = request.side;
        order.limit = is_market ? std::nullopt : std::optional<Price>(request.price);
        order.leaves = request.quantity;

        if (request.tif == TimeInForce::FillOrKill) {
            ReferenceBook trial = *this;
            Working probe = order;
            if (trial.run(probe) || probe.leaves > 0) {
                return false;
            }
        }
        const bool stopped = request.post_only ? false : run(order);
        if (!stopped && order.leaves > 0 && request.tif == TimeInForce::GoodTillCancel) {
            RestingOrder resting{};
            resting.id = id;
            resting.participant = request.participant;
            resting.stp = request.stp;
            resting.post_only = request.post_only;
            resting.side = request.side;
            resting.price = request.price;
            resting.quantity = request.quantity;
            resting.executed = request.quantity - order.leaves;
            resting.display = request.display_quantity;
            split(resting, order.leaves);
            resting.sequence = ++sequence_;
            book_.push_back(resting);
        }
        return true;
    }

    bool cancel(OrderId id) {
        const auto it = locate(id);
        if (it == book_.end()) {
            return false;
        }
        book_.erase(it);
        return true;
    }

    bool replace(OrderId id, Price price, Quantity quantity) {
        auto it = locate(id);
        if (it == book_.end() || quantity <= it->executed) {
            return false;
        }
        const Quantity new_leaves = quantity - it->executed;
        const Quantity old_leaves = it->visible + it->hidden;
        if (price == it->price && new_leaves <= old_leaves) {
            it->visible = std::min(it->visible, new_leaves);
            it->hidden = new_leaves - it->visible;
            it->quantity = quantity;
            return true;
        }
        if (it->post_only && crosses_opposite(it->side, price)) {
            return false;
        }
        RestingOrder order = *it;
        book_.erase(it);
        order.price = price;
        order.quantity = quantity;

        Working working{};
        working.id = order.id;
        working.participant = order.participant;
        working.stp = order.stp;
        working.side = order.side;
        working.limit = price;
        working.leaves = new_leaves;
        const bool stopped = order.post_only ? false : run(working);
        if (!stopped && working.leaves > 0) {
            order.executed = quantity - working.leaves;
            split(order, working.leaves);
            order.sequence = ++sequence_;
            book_.push_back(order);
        }
        return true;
    }

    [[nodiscard]] bool contains(OrderId id) const {
        return std::any_of(book_.begin(), book_.end(),
                           [id](const RestingOrder& o) { return o.id == id; });
    }

    /// All resting orders on `side` in priority order.
    [[nodiscard]] std::vector<RestingOrder> orders(Side side) const {
        std::vector<RestingOrder> out;
        for (const RestingOrder& o : book_) {
            if (o.side == side) {
                out.push_back(o);
            }
        }
        std::sort(out.begin(), out.end(),
                  [](const RestingOrder& a, const RestingOrder& b) { return better(a, b); });
        return out;
    }

    [[nodiscard]] std::vector<OrderId> live_ids() const {
        std::vector<OrderId> ids;
        for (const RestingOrder& o : book_) {
            ids.push_back(o.id);
        }
        return ids;
    }

  private:
    struct Working {
        OrderId id = 0;
        ParticipantId participant = 0;
        SelfTradePrevention stp = SelfTradePrevention::None;
        Side side = Side::Buy;
        std::optional<Price> limit;
        Quantity leaves = 0;
    };

    static bool better(const RestingOrder& a, const RestingOrder& b) {
        if (a.price != b.price) {
            return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
        }
        return a.sequence < b.sequence;
    }

    static void split(RestingOrder& order, Quantity leaves) {
        order.visible = order.display > 0 ? std::min(order.display, leaves) : leaves;
        order.hidden = leaves - order.visible;
    }

    std::vector<RestingOrder>::iterator locate(OrderId id) {
        return std::find_if(book_.begin(), book_.end(),
                            [id](const RestingOrder& o) { return o.id == id; });
    }

    [[nodiscard]] bool has_side(Side side) const {
        return std::any_of(book_.begin(), book_.end(),
                           [side](const RestingOrder& o) { return o.side == side; });
    }

    [[nodiscard]] bool crosses_opposite(Side side, Price price) const {
        return std::any_of(book_.begin(), book_.end(), [&](const RestingOrder& o) {
            return o.side != side && price_acceptable(side, o.price, price);
        });
    }

    std::vector<RestingOrder>::iterator best_opposite(Side side) {
        auto best = book_.end();
        for (auto it = book_.begin(); it != book_.end(); ++it) {
            if (it->side != side && (best == book_.end() || better(*it, *best))) {
                best = it;
            }
        }
        return best;
    }

    /// Matches `order` against the book. Returns true if self-trade prevention stopped it.
    bool run(Working& order) {
        const bool stp_active = order.stp != SelfTradePrevention::None && order.participant != 0;
        while (order.leaves > 0) {
            auto it = best_opposite(order.side);
            if (it == book_.end()) {
                break;
            }
            if (order.limit && !price_acceptable(order.side, it->price, *order.limit)) {
                break;
            }
            if (stp_active && it->participant == order.participant) {
                if (order.stp == SelfTradePrevention::CancelIncoming) {
                    return true;
                }
                book_.erase(it);
                if (order.stp == SelfTradePrevention::CancelBoth) {
                    return true;
                }
                continue;
            }
            const Quantity quantity = std::min(order.leaves, it->visible);
            fills.push_back(Fill{order.id, it->id, it->price, quantity});
            order.leaves -= quantity;
            it->visible -= quantity;
            it->executed += quantity;
            if (it->visible == 0) {
                if (it->hidden > 0) {
                    split(*it, it->hidden);
                    it->sequence = ++sequence_;
                } else {
                    book_.erase(it);
                }
            }
        }
        return false;
    }

    std::vector<RestingOrder> book_;
    std::uint64_t sequence_ = 0;
};

}  // namespace quill::test
