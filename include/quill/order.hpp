#pragma once

#include <cassert>
#include <cstddef>

#include "quill/types.hpp"

namespace quill {

class PriceLevel;

/// A resting (or in-flight aggressive) order.
///
/// Orders are intrusive list nodes so that a price level can unlink one in O(1) given only
/// the pointer stored in the id index. Quantity is split into the displayed portion
/// (`visible`) and the reserve (`hidden`); only iceberg orders ever carry a reserve.
struct Order {
    OrderId id = kInvalidOrderId;
    std::uint64_t client_tag = 0;
    InstrumentId instrument = 0;
    ParticipantId participant = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GoodTillCancel;
    SelfTradePrevention stp = SelfTradePrevention::None;
    bool post_only = false;

    Price price = 0;
    /// Total order quantity as last specified by the client (new or replace).
    Quantity quantity = 0;
    Quantity executed = 0;
    /// Displayed quantity currently available to match.
    Quantity visible = 0;
    /// Reserve quantity that refills `visible` when it is exhausted.
    Quantity hidden = 0;
    /// Peak size for iceberg orders; zero means the whole order is displayed.
    Quantity display = 0;
    Timestamp entry_time = 0;

    Order* prev = nullptr;
    Order* next = nullptr;
    PriceLevel* level = nullptr;

    [[nodiscard]] Quantity leaves() const noexcept { return visible + hidden; }
    [[nodiscard]] bool is_iceberg() const noexcept { return display > 0; }

    /// Splits `leaves` into displayed and reserve quantity according to the peak size.
    void load_leaves(Quantity leaves) noexcept {
        assert(leaves >= 0);
        visible = is_iceberg() ? (leaves < display ? leaves : display) : leaves;
        hidden = leaves - visible;
    }

    /// Moves the next tranche of reserve into the displayed quantity.
    void replenish() noexcept {
        assert(visible == 0 && hidden > 0);
        load_leaves(hidden);
    }
};

/// FIFO queue of orders sharing one price on one side of the book.
class PriceLevel {
  public:
    PriceLevel(Side side, Price price) noexcept : side_(side), price_(price) {}

    [[nodiscard]] Side side() const noexcept { return side_; }
    [[nodiscard]] Price price() const noexcept { return price_; }
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
    [[nodiscard]] Order* front() const noexcept { return head_; }
    [[nodiscard]] Order* back() const noexcept { return tail_; }
    [[nodiscard]] std::size_t order_count() const noexcept { return count_; }
    /// Sum of displayed quantity, i.e. what a market data consumer sees.
    [[nodiscard]] Quantity visible_quantity() const noexcept { return visible_; }
    /// Sum of displayed plus reserve quantity, i.e. what can actually trade here.
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_; }

    void push_back(Order* order) noexcept {
        assert(order->level == nullptr && order->prev == nullptr && order->next == nullptr);
        order->level = this;
        order->prev = tail_;
        if (tail_ != nullptr) {
            tail_->next = order;
        } else {
            head_ = order;
        }
        tail_ = order;
        ++count_;
        visible_ += order->visible;
        total_ += order->leaves();
    }

    void remove(Order* order) noexcept {
        assert(order->level == this);
        if (order->prev != nullptr) {
            order->prev->next = order->next;
        } else {
            head_ = order->next;
        }
        if (order->next != nullptr) {
            order->next->prev = order->prev;
        } else {
            tail_ = order->prev;
        }
        visible_ -= order->visible;
        total_ -= order->leaves();
        --count_;
        order->prev = nullptr;
        order->next = nullptr;
        order->level = nullptr;
    }

    /// Accounts for `quantity` leaving the displayed portion of a member order. The caller
    /// updates the order itself.
    void reduce_visible(Quantity quantity) noexcept {
        visible_ -= quantity;
        total_ -= quantity;
        assert(visible_ >= 0 && total_ >= 0);
    }

    /// Accounts for `quantity` leaving the reserve portion of a member order.
    void reduce_hidden(Quantity quantity) noexcept {
        total_ -= quantity;
        assert(total_ >= visible_);
    }

    /// Accounts for a member order moving `quantity` from reserve to display.
    void reveal(Quantity quantity) noexcept {
        visible_ += quantity;
        assert(visible_ <= total_);
    }

  private:
    Side side_;
    Price price_;
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    std::size_t count_ = 0;
    Quantity visible_ = 0;
    Quantity total_ = 0;
};

}  // namespace quill
