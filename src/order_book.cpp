#include "quill/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <utility>

namespace quill {

OrderBook::OrderBook(InstrumentId id, InstrumentSpec spec, EventListener& listener,
                     MatchId& match_sequence)
    : id_(id), spec_(std::move(spec)), listener_(&listener), match_sequence_(&match_sequence) {
    assert(spec_.tick_size > 0);
    assert(spec_.lot_size > 0);
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    const PriceLevel* level = bids_.best();
    return level != nullptr ? std::optional<Price>(level->price()) : std::nullopt;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    const PriceLevel* level = asks_.best();
    return level != nullptr ? std::optional<Price>(level->price()) : std::nullopt;
}

const Order* OrderBook::find(OrderId order_id) const noexcept {
    Order* const* slot = index_.find(order_id);
    return slot != nullptr ? *slot : nullptr;
}

std::vector<LevelSnapshot> OrderBook::depth(Side side, std::size_t max_levels) const {
    const PriceLadder& book = ladder(side);
    std::vector<LevelSnapshot> out;
    out.reserve(std::min(max_levels, book.size()));
    for (std::size_t i = 0; i < max_levels; ++i) {
        const PriceLevel* level = book.at_depth(i);
        if (level == nullptr) {
            break;
        }
        out.push_back(LevelSnapshot{level->price(), level->visible_quantity(),
                                    level->total_quantity(), level->order_count()});
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------------------

RejectReason OrderBook::validate_price(Price price) const noexcept {
    if (price <= 0 || price > kMaxPrice) {
        return RejectReason::InvalidPrice;
    }
    if (price % spec_.tick_size != 0) {
        return RejectReason::PriceNotOnTick;
    }
    if (spec_.collar_bps > 0 && last_trade_) {
        const Price band = *last_trade_ * static_cast<Price>(spec_.collar_bps) / 10'000;
        if (price < *last_trade_ - band || price > *last_trade_ + band) {
            return RejectReason::PriceOutsideCollar;
        }
    }
    return RejectReason::None;
}

RejectReason OrderBook::validate_quantity(Quantity quantity) const noexcept {
    if (quantity <= 0 || quantity > spec_.max_order_quantity || quantity > kMaxQuantity) {
        return RejectReason::InvalidQuantity;
    }
    if (quantity % spec_.lot_size != 0) {
        return RejectReason::QuantityNotOnLot;
    }
    return RejectReason::None;
}

RejectReason OrderBook::validate_new(const NewOrderRequest& request) const noexcept {
    if (const RejectReason r = validate_quantity(request.quantity); r != RejectReason::None) {
        return r;
    }
    const bool is_market = request.type == OrderType::Market;
    if (is_market && request.tif == TimeInForce::GoodTillCancel) {
        // A market order has no price to rest at.
        return RejectReason::InvalidTimeInForce;
    }
    if (request.post_only && (is_market || request.tif != TimeInForce::GoodTillCancel)) {
        // Post-only exists to add liquidity; it is meaningless for orders that never rest.
        return RejectReason::InvalidTimeInForce;
    }
    if (request.display_quantity != 0) {
        if (is_market || request.tif != TimeInForce::GoodTillCancel) {
            return RejectReason::InvalidTimeInForce;
        }
        if (request.display_quantity < 0 || request.display_quantity >= request.quantity ||
            request.display_quantity % spec_.lot_size != 0) {
            return RejectReason::InvalidDisplayQuantity;
        }
    }
    if (!is_market) {
        return validate_price(request.price);
    }
    return RejectReason::None;
}

std::optional<Price> OrderBook::market_limit(Side side) const noexcept {
    if (spec_.collar_bps == 0 || !last_trade_) {
        return std::nullopt;
    }
    const Price band = *last_trade_ * static_cast<Price>(spec_.collar_bps) / 10'000;
    return side == Side::Buy ? *last_trade_ + band : *last_trade_ - band;
}

bool OrderBook::would_cross(Side side, Price price) const noexcept {
    if (side == Side::Buy) {
        const auto ask = best_ask();
        return ask && crosses(price, *ask);
    }
    const auto bid = best_bid();
    return bid && crosses(*bid, price);
}

bool OrderBook::fill_or_kill_fillable(const NewOrderRequest& request,
                                      std::optional<Price> limit) const noexcept {
    Quantity needed = request.quantity;
    const bool stp_active = request.stp != SelfTradePrevention::None && request.participant != 0;
    // With CancelIncoming or CancelBoth, meeting one of our own orders ends the match, so
    // only liquidity queued ahead of it can count. Reserve quantity of icebergs ahead of
    // it refills at the back of the queue, behind our own order, so only the displayed
    // part of those counts.
    const bool self_trade_stops = stp_active && request.stp != SelfTradePrevention::CancelResting;

    const auto levels = ladder(opposite(request.side)).levels();
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
        const PriceLevel& level = **it;
        if (limit && !price_acceptable(request.side, level.price(), *limit)) {
            break;
        }
        if (!stp_active) {
            needed -= level.total_quantity();
        } else {
            Quantity ahead_visible = 0;
            Quantity usable = 0;
            bool blocked = false;
            for (const Order* order = level.front(); order != nullptr; order = order->next) {
                if (order->participant == request.participant) {
                    if (self_trade_stops) {
                        blocked = true;
                        break;
                    }
                    continue;  // CancelResting: our own orders are removed, not traded.
                }
                ahead_visible += order->visible;
                usable += order->leaves();
            }
            if (blocked) {
                return ahead_visible >= needed;
            }
            needed -= usable;
        }
        if (needed <= 0) {
            return true;
        }
    }
    return needed <= 0;
}

// ---------------------------------------------------------------------------------------
// Request handling
// ---------------------------------------------------------------------------------------

bool OrderBook::submit(const NewOrderRequest& request, OrderId order_id, Timestamp now) {
    if (const RejectReason r = validate_new(request); r != RejectReason::None) {
        reject(RequestKind::NewOrder, kInvalidOrderId, request.client_tag, r, now);
        return false;
    }
    if (index_.contains(order_id)) {
        reject(RequestKind::NewOrder, kInvalidOrderId, request.client_tag,
               RejectReason::DuplicateOrderId, now);
        return false;
    }

    const bool is_market = request.type == OrderType::Market;
    if (is_market && ladder(opposite(request.side)).empty()) {
        reject(RequestKind::NewOrder, kInvalidOrderId, request.client_tag,
               RejectReason::NoLiquidity, now);
        return false;
    }
    if (request.post_only && would_cross(request.side, request.price)) {
        reject(RequestKind::NewOrder, kInvalidOrderId, request.client_tag,
               RejectReason::PostOnlyWouldCross, now);
        return false;
    }

    const std::optional<Price> limit =
        is_market ? market_limit(request.side) : std::optional<Price>(request.price);
    if (request.tif == TimeInForce::FillOrKill && !fill_or_kill_fillable(request, limit)) {
        reject(RequestKind::NewOrder, kInvalidOrderId, request.client_tag,
               RejectReason::FillOrKillUnfillable, now);
        return false;
    }

    listener_->on_accepted(OrderAccepted{id_, order_id, request.client_tag, request.participant,
                                         request.side, request.type, request.tif,
                                         is_market ? 0 : request.price, request.quantity, now});

    Order& order = *orders_.acquire();
    order.id = order_id;
    order.client_tag = request.client_tag;
    order.instrument = id_;
    order.participant = request.participant;
    order.side = request.side;
    order.type = request.type;
    order.tif = request.tif;
    order.stp = request.stp;
    order.post_only = request.post_only;
    order.price = is_market ? 0 : request.price;
    order.quantity = request.quantity;
    order.display = request.display_quantity;
    order.entry_time = now;
    order.load_leaves(request.quantity);

    // Post-only orders were checked not to cross, so they skip straight to resting.
    if (!request.post_only && match(order, limit, now) == MatchResult::StoppedBySelfTrade) {
        cancel_remainder(order, CancelReason::SelfTradePrevention, now);
        release(order);
        return true;
    }

    if (order.leaves() == 0) {
        release(order);
    } else if (order.tif == TimeInForce::GoodTillCancel) {
        index_.insert(order.id, &order);
        rest(order, now);
    } else {
        cancel_remainder(order,
                         is_market ? CancelReason::MarketOrderRemainder
                                   : CancelReason::ImmediateOrCancelRemainder,
                         now);
        release(order);
    }
    return true;
}

void OrderBook::cancel(const CancelRequest& request, Timestamp now) {
    Order** slot = index_.find(request.order_id);
    if (slot == nullptr) {
        reject(RequestKind::Cancel, request.order_id, request.client_tag,
               RejectReason::UnknownOrder, now);
        return;
    }
    Order& order = **slot;
    cancel_resting(order, request.client_tag, CancelReason::UserRequested, now);
}

void OrderBook::replace(const ReplaceRequest& request, Timestamp now) {
    Order** slot = index_.find(request.order_id);
    if (slot == nullptr) {
        reject(RequestKind::Replace, request.order_id, request.client_tag,
               RejectReason::UnknownOrder, now);
        return;
    }
    Order& order = **slot;

    RejectReason reason = validate_quantity(request.quantity);
    if (reason == RejectReason::None && request.price != order.price) {
        // An unchanged price is always acceptable even if the collar has since moved
        // away from it; otherwise a client could never shrink a stale order.
        reason = validate_price(request.price);
    }
    if (reason == RejectReason::None && request.quantity <= order.executed) {
        reason = RejectReason::QuantityBelowExecuted;
    }
    const Quantity new_leaves = request.quantity - order.executed;
    const bool keeps_priority = request.price == order.price && new_leaves <= order.leaves();
    if (reason == RejectReason::None && !keeps_priority && order.post_only &&
        would_cross(order.side, request.price)) {
        reason = RejectReason::PostOnlyWouldCross;
    }
    if (reason != RejectReason::None) {
        reject(RequestKind::Replace, request.order_id, request.client_tag, reason, now);
        return;
    }

    if (keeps_priority) {
        // Size-down in place. Reserve quantity is consumed before displayed quantity, so a
        // reduction only becomes visible once the reserve is gone.
        PriceLevel& level = *order.level;
        const Quantity old_visible = order.visible;
        const Quantity old_hidden = order.hidden;
        const Quantity new_visible = std::min(old_visible, new_leaves);
        const Quantity new_hidden = new_leaves - new_visible;
        order.visible = new_visible;
        order.hidden = new_hidden;
        order.quantity = request.quantity;
        level.reduce_visible(old_visible - new_visible);
        level.reduce_hidden(old_hidden - new_hidden);
        listener_->on_replaced(OrderReplaced{id_, order.id, request.client_tag, order.price,
                                             order.quantity, order.leaves(), true, now});
        if (new_visible != old_visible) {
            listener_->on_book_reduce(
                BookOrderReduced{id_, order.id, old_visible - new_visible, now});
        }
        return;
    }

    // Anything else loses time priority: pull the order, re-price it, and run it through
    // matching exactly like a new order before it rests again.
    unlink(order);
    listener_->on_book_delete(BookOrderDeleted{id_, order.id, now});
    order.price = request.price;
    order.quantity = request.quantity;
    order.entry_time = now;
    order.load_leaves(new_leaves);
    listener_->on_replaced(OrderReplaced{id_, order.id, request.client_tag, order.price,
                                         order.quantity, order.leaves(), false, now});

    const MatchResult result = order.post_only ? MatchResult::Done : match(order, order.price, now);
    if (result == MatchResult::StoppedBySelfTrade) {
        cancel_remainder(order, CancelReason::SelfTradePrevention, now);
        index_.erase(order.id);
        release(order);
    } else if (order.leaves() == 0) {
        index_.erase(order.id);
        release(order);
    } else {
        rest(order, now);
    }
}

// ---------------------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------------------

OrderBook::MatchResult OrderBook::match(Order& aggressor, std::optional<Price> limit,
                                        Timestamp now) {
    PriceLadder& book = ladder_for(opposite(aggressor.side));
    const bool stp_active =
        aggressor.stp != SelfTradePrevention::None && aggressor.participant != 0;

    while (aggressor.leaves() > 0) {
        PriceLevel* level = book.best();
        if (level == nullptr) {
            break;
        }
        if (limit && !price_acceptable(aggressor.side, level->price(), *limit)) {
            break;
        }
        Order& resting = *level->front();
        if (stp_active && resting.participant == aggressor.participant) {
            switch (aggressor.stp) {
                case SelfTradePrevention::CancelResting:
                    cancel_resting(resting, resting.client_tag, CancelReason::SelfTradePrevention,
                                   now);
                    continue;
                case SelfTradePrevention::CancelBoth:
                    cancel_resting(resting, resting.client_tag, CancelReason::SelfTradePrevention,
                                   now);
                    return MatchResult::StoppedBySelfTrade;
                case SelfTradePrevention::CancelIncoming:
                    return MatchResult::StoppedBySelfTrade;
                case SelfTradePrevention::None:
                    break;
            }
        }
        fill(aggressor, resting, now);
    }
    return MatchResult::Done;
}

void OrderBook::fill(Order& aggressor, Order& resting, Timestamp now) {
    PriceLevel& level = *resting.level;
    const Price price = level.price();
    const Quantity quantity = std::min(aggressor.leaves(), resting.visible);
    assert(quantity > 0);

    const MatchId match_id = ++*match_sequence_;
    resting.visible -= quantity;
    resting.executed += quantity;
    level.reduce_visible(quantity);
    aggressor.executed += quantity;
    // The aggressor is not on a level yet, so its display split is irrelevant until it
    // rests; keep the whole remainder in `visible` to make leaves() cheap and exact.
    aggressor.visible = aggressor.quantity - aggressor.executed;
    aggressor.hidden = 0;
    last_trade_ = price;

    listener_->on_trade(Trade{id_, match_id, aggressor.id, resting.id, aggressor.participant,
                              resting.participant, aggressor.side, price, quantity,
                              aggressor.leaves(), resting.leaves(), now});
    listener_->on_book_execute(BookOrderExecuted{id_, resting.id, quantity, match_id, now});

    if (resting.visible > 0) {
        return;
    }
    if (resting.hidden > 0) {
        // Iceberg refill: the new tranche joins the back of the queue and is published as
        // a fresh displayed order.
        level.remove(&resting);
        resting.replenish();
        level.push_back(&resting);
        listener_->on_book_add(
            BookOrderAdded{id_, resting.id, resting.side, price, resting.visible, now});
        return;
    }
    unlink(resting);
    index_.erase(resting.id);
    release(resting);
}

void OrderBook::rest(Order& order, Timestamp now) {
    PriceLevel* level = ladder_for(order.side).find_or_insert(order.price);
    order.load_leaves(order.quantity - order.executed);
    level->push_back(&order);
    listener_->on_book_add(
        BookOrderAdded{id_, order.id, order.side, order.price, order.visible, now});
}

void OrderBook::unlink(Order& order) noexcept {
    PriceLevel* level = order.level;
    assert(level != nullptr);
    level->remove(&order);
    if (level->empty()) {
        ladder_for(order.side).erase(level);
    }
}

void OrderBook::cancel_resting(Order& order, std::uint64_t client_tag, CancelReason reason,
                               Timestamp now) {
    const Quantity leaves = order.leaves();
    unlink(order);
    index_.erase(order.id);
    listener_->on_book_delete(BookOrderDeleted{id_, order.id, now});
    listener_->on_cancelled(OrderCancelled{id_, order.id, client_tag, leaves, reason, now});
    release(order);
}

void OrderBook::cancel_remainder(Order& order, CancelReason reason, Timestamp now) {
    listener_->on_cancelled(
        OrderCancelled{id_, order.id, order.client_tag, order.leaves(), reason, now});
}

void OrderBook::release(Order& order) noexcept {
    orders_.release(&order);
}

void OrderBook::reject(RequestKind kind, OrderId order_id, std::uint64_t client_tag,
                       RejectReason reason, Timestamp now) {
    listener_->on_rejected(OrderRejected{id_, order_id, client_tag, kind, reason, now});
}

// ---------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------

std::optional<std::string> OrderBook::check_invariants() const {
    std::ostringstream err;
    std::size_t resting = 0;

    for (const PriceLadder* book : {&bids_, &asks_}) {
        const auto levels = book->levels();
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const PriceLevel& level = *levels[i];
            if (level.side() != book->side()) {
                err << "level " << level.price() << " filed under the wrong side";
                return err.str();
            }
            if (level.empty()) {
                err << "empty level left in ladder at " << level.price();
                return err.str();
            }
            if (i > 0) {
                const Price prev = levels[i - 1]->price();
                const bool ordered =
                    book->side() == Side::Buy ? prev < level.price() : prev > level.price();
                if (!ordered) {
                    err << "ladder out of order at " << level.price();
                    return err.str();
                }
            }
            Quantity visible = 0;
            Quantity total = 0;
            std::size_t count = 0;
            const Order* prev_order = nullptr;
            for (const Order* order = level.front(); order != nullptr; order = order->next) {
                if (order->prev != prev_order || order->level != &level) {
                    err << "broken links around order " << order->id;
                    return err.str();
                }
                if (order->price != level.price() || order->side != level.side()) {
                    err << "order " << order->id << " sits on the wrong level";
                    return err.str();
                }
                if (order->visible <= 0 || order->hidden < 0) {
                    err << "order " << order->id << " has non-positive displayed quantity";
                    return err.str();
                }
                if (!order->is_iceberg() && order->hidden != 0) {
                    err << "non-iceberg order " << order->id << " carries reserve";
                    return err.str();
                }
                if (order->is_iceberg() && order->visible > order->display) {
                    err << "iceberg " << order->id << " displays more than its peak";
                    return err.str();
                }
                if (order->executed + order->leaves() != order->quantity) {
                    err << "order " << order->id << " quantity does not add up";
                    return err.str();
                }
                const Order* indexed = find(order->id);
                if (indexed != order) {
                    err << "order " << order->id << " missing from index";
                    return err.str();
                }
                visible += order->visible;
                total += order->leaves();
                ++count;
                prev_order = order;
            }
            if (prev_order != level.back()) {
                err << "level tail mismatch at " << level.price();
                return err.str();
            }
            if (visible != level.visible_quantity() || total != level.total_quantity() ||
                count != level.order_count()) {
                err << "level aggregates stale at " << level.price();
                return err.str();
            }
            resting += count;
        }
    }
    if (resting != index_.size()) {
        err << "index holds " << index_.size() << " orders but book holds " << resting;
        return err.str();
    }
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (bid && ask && crosses(*bid, *ask)) {
        err << "book is crossed: " << *bid << " >= " << *ask;
        return err.str();
    }
    return std::nullopt;
}

}  // namespace quill
