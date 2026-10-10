#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

#include "quill/object_pool.hpp"
#include "quill/order.hpp"

namespace quill {

/// One side of an order book: the set of non-empty price levels, kept sorted from the
/// least to the most aggressive price.
///
/// Keeping the best level at the *back* of a contiguous vector means that the hot
/// operations (look at the touch, add or remove a level near the touch) touch the end of
/// the array, where insertion and erasure move very few elements. Lookups for arbitrary
/// prices are a binary search over a cache-friendly array of pointers.
class PriceLadder {
  public:
    explicit PriceLadder(Side side) : side_(side), pool_(256) { levels_.reserve(256); }

    PriceLadder(const PriceLadder&) = delete;
    PriceLadder& operator=(const PriceLadder&) = delete;
    PriceLadder(PriceLadder&&) noexcept = default;
    PriceLadder& operator=(PriceLadder&&) noexcept = default;
    ~PriceLadder() = default;

    [[nodiscard]] Side side() const noexcept { return side_; }
    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

    /// The most aggressive level (highest bid or lowest ask), or nullptr.
    [[nodiscard]] PriceLevel* best() const noexcept {
        return levels_.empty() ? nullptr : levels_.back();
    }

    /// The level at `depth` positions behind the touch (0 is the best level).
    [[nodiscard]] PriceLevel* at_depth(std::size_t depth) const noexcept {
        return depth < levels_.size() ? levels_[levels_.size() - 1 - depth] : nullptr;
    }

    [[nodiscard]] PriceLevel* find(Price price) const noexcept {
        const auto it = lower_bound(price);
        return (it != levels_.end() && (*it)->price() == price) ? *it : nullptr;
    }

    /// Returns the level for `price`, creating an empty one in sorted position if needed.
    PriceLevel* find_or_insert(Price price) {
        // Fast path: most new liquidity arrives at or just behind the touch.
        if (!levels_.empty() && levels_.back()->price() == price) {
            return levels_.back();
        }
        auto it = lower_bound(price);
        if (it != levels_.end() && (*it)->price() == price) {
            return *it;
        }
        PriceLevel* level = pool_.acquire(side_, price);
        levels_.insert(it, level);
        return level;
    }

    /// Removes an empty level from the ladder and returns it to the pool.
    void erase(PriceLevel* level) noexcept {
        assert(level->empty());
        if (!levels_.empty() && levels_.back() == level) {
            levels_.pop_back();
        } else {
            const auto it = lower_bound(level->price());
            assert(it != levels_.end() && *it == level);
            levels_.erase(it);
        }
        pool_.release(level);
    }

    /// Levels ordered from least to most aggressive.
    [[nodiscard]] std::span<PriceLevel* const> levels() const noexcept { return levels_; }

  private:
    /// Monotone key under which the vector is sorted ascending: for bids a higher price is
    /// better, for asks a lower one is, so asks are ranked by the negated price.
    [[nodiscard]] Price rank(Price price) const noexcept {
        return side_ == Side::Buy ? price : -price;
    }

    [[nodiscard]] std::vector<PriceLevel*>::const_iterator lower_bound(Price price) const {
        const Price key = rank(price);
        return std::lower_bound(
            levels_.begin(), levels_.end(), key,
            [this](const PriceLevel* level, Price k) { return rank(level->price()) < k; });
    }

    Side side_;
    ObjectPool<PriceLevel> pool_;
    std::vector<PriceLevel*> levels_;
};

}  // namespace quill
