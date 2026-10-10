#include "quill/price_ladder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

namespace quill {
namespace {

std::vector<Price> prices_best_first(const PriceLadder& ladder) {
    std::vector<Price> out;
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        out.push_back(ladder.at_depth(i)->price());
    }
    return out;
}

TEST(PriceLadder, BidsAreBestHighest) {
    PriceLadder bids(Side::Buy);
    for (Price p : {100, 300, 200, 250, 50}) {
        bids.find_or_insert(p);
    }
    EXPECT_EQ(bids.best()->price(), 300);
    EXPECT_EQ(prices_best_first(bids), (std::vector<Price>{300, 250, 200, 100, 50}));
}

TEST(PriceLadder, AsksAreBestLowest) {
    PriceLadder asks(Side::Sell);
    for (Price p : {100, 300, 200, 250, 50}) {
        asks.find_or_insert(p);
    }
    EXPECT_EQ(asks.best()->price(), 50);
    EXPECT_EQ(prices_best_first(asks), (std::vector<Price>{50, 100, 200, 250, 300}));
}

TEST(PriceLadder, FindOrInsertIsIdempotent) {
    PriceLadder bids(Side::Buy);
    PriceLevel* a = bids.find_or_insert(100);
    PriceLevel* b = bids.find_or_insert(100);
    EXPECT_EQ(a, b);
    EXPECT_EQ(bids.size(), 1U);
    EXPECT_EQ(a->side(), Side::Buy);
}

TEST(PriceLadder, FindMissingReturnsNull) {
    PriceLadder asks(Side::Sell);
    asks.find_or_insert(100);
    EXPECT_EQ(asks.find(101), nullptr);
    EXPECT_NE(asks.find(100), nullptr);
    EXPECT_EQ(asks.at_depth(1), nullptr);
}

TEST(PriceLadder, EraseBestAndInterior) {
    PriceLadder bids(Side::Buy);
    for (Price p : {100, 200, 300}) {
        bids.find_or_insert(p);
    }
    bids.erase(bids.find(300));
    EXPECT_EQ(bids.best()->price(), 200);
    bids.erase(bids.find(100));
    EXPECT_EQ(prices_best_first(bids), (std::vector<Price>{200}));
    bids.erase(bids.best());
    EXPECT_TRUE(bids.empty());
    EXPECT_EQ(bids.best(), nullptr);
}

TEST(PriceLadder, StaysSortedUnderRandomChurn) {
    for (Side side : {Side::Buy, Side::Sell}) {
        PriceLadder ladder(side);
        std::set<Price> reference;
        std::mt19937 rng(7);
        for (int step = 0; step < 20'000; ++step) {
            const Price price = static_cast<Price>(rng() % 500) * 100;
            if (rng() % 2 == 0) {
                ladder.find_or_insert(price);
                reference.insert(price);
            } else if (PriceLevel* level = ladder.find(price)) {
                ladder.erase(level);
                reference.erase(price);
            }
        }
        std::vector<Price> expected(reference.begin(), reference.end());
        if (side == Side::Buy) {
            std::reverse(expected.begin(), expected.end());
        }
        EXPECT_EQ(prices_best_first(ladder), expected);
    }
}

TEST(PriceLevel, FifoLinkingAndAggregates) {
    PriceLevel level(Side::Buy, 100);
    Order a{};
    a.id = 1;
    a.visible = 10;
    Order b{};
    b.id = 2;
    b.visible = 5;
    b.hidden = 20;
    b.display = 5;
    Order c{};
    c.id = 3;
    c.visible = 7;

    level.push_back(&a);
    level.push_back(&b);
    level.push_back(&c);
    EXPECT_EQ(level.order_count(), 3U);
    EXPECT_EQ(level.visible_quantity(), 22);
    EXPECT_EQ(level.total_quantity(), 42);
    EXPECT_EQ(level.front(), &a);
    EXPECT_EQ(level.back(), &c);

    level.remove(&b);
    EXPECT_EQ(a.next, &c);
    EXPECT_EQ(c.prev, &a);
    EXPECT_EQ(b.level, nullptr);
    EXPECT_EQ(level.visible_quantity(), 17);
    EXPECT_EQ(level.total_quantity(), 17);

    level.remove(&a);
    EXPECT_EQ(level.front(), &c);
    level.remove(&c);
    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.back(), nullptr);
}

TEST(Order, LoadLeavesSplitsIcebergs) {
    Order order{};
    order.display = 100;
    order.load_leaves(250);
    EXPECT_EQ(order.visible, 100);
    EXPECT_EQ(order.hidden, 150);
    order.visible = 0;
    order.replenish();
    EXPECT_EQ(order.visible, 100);
    EXPECT_EQ(order.hidden, 50);
    order.visible = 0;
    order.replenish();
    EXPECT_EQ(order.visible, 50);
    EXPECT_EQ(order.hidden, 0);

    Order plain{};
    plain.load_leaves(42);
    EXPECT_EQ(plain.visible, 42);
    EXPECT_EQ(plain.hidden, 0);
}

}  // namespace
}  // namespace quill
