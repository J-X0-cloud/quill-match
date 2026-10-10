#include <gtest/gtest.h>

#include "test_support.hpp"

namespace quill::test {
namespace {

using OrderBookTest = EngineFixture;

TEST_F(OrderBookTest, RestingOrdersFormTheTouch) {
    const OrderId bid = buy(px(99.00), 100);
    const OrderId ask = sell(px(101.00), 50);
    EXPECT_NE(bid, kInvalidOrderId);
    EXPECT_NE(ask, kInvalidOrderId);
    EXPECT_EQ(book().best_bid(), px(99.00));
    EXPECT_EQ(book().best_ask(), px(101.00));
    EXPECT_EQ(book().order_count(), 2U);
    EXPECT_TRUE(events.trades.empty());
    EXPECT_EQ(events.sequence, "AaAa");
    expect_consistent();
}

TEST_F(OrderBookTest, EmptyBookHasNoTouch) {
    EXPECT_FALSE(book().best_bid().has_value());
    EXPECT_FALSE(book().best_ask().has_value());
    EXPECT_FALSE(book().last_trade_price().has_value());
    EXPECT_TRUE(book().depth(Side::Buy, 5).empty());
}

TEST_F(OrderBookTest, CrossingOrderTradesAtRestingPrice) {
    const OrderId ask = sell(px(100.00), 100);
    const OrderId bid = buy(px(101.00), 100);

    ASSERT_EQ(events.trades.size(), 1U);
    const Trade& trade = events.trades[0];
    EXPECT_EQ(trade.price, px(100.00));  // price improvement goes to the aggressor
    EXPECT_EQ(trade.quantity, 100);
    EXPECT_EQ(trade.aggressor_id, bid);
    EXPECT_EQ(trade.resting_id, ask);
    EXPECT_EQ(trade.aggressor_side, Side::Buy);
    EXPECT_EQ(trade.aggressor_leaves, 0);
    EXPECT_EQ(trade.resting_leaves, 0);
    EXPECT_EQ(book().order_count(), 0U);
    EXPECT_EQ(book().last_trade_price(), px(100.00));
    expect_consistent();
}

TEST_F(OrderBookTest, PartialFillLeavesRemainderResting) {
    sell(px(100.00), 30);
    const OrderId bid = buy(px(100.00), 100);

    ASSERT_EQ(events.trades.size(), 1U);
    EXPECT_EQ(events.trades[0].quantity, 30);
    EXPECT_EQ(events.trades[0].aggressor_leaves, 70);
    const Order* resting = book().find(bid);
    ASSERT_NE(resting, nullptr);
    EXPECT_EQ(resting->leaves(), 70);
    EXPECT_EQ(resting->executed, 30);
    EXPECT_EQ(book().best_bid(), px(100.00));
    EXPECT_FALSE(book().best_ask().has_value());
    expect_consistent();
}

TEST_F(OrderBookTest, AggressorSweepsMultipleLevelsInPriceOrder) {
    sell(px(100.02), 10);
    sell(px(100.00), 10);
    sell(px(100.01), 10);
    sell(px(100.03), 10);
    buy(px(100.02), 35);

    ASSERT_EQ(events.trades.size(), 3U);
    EXPECT_EQ(events.trades[0].price, px(100.00));
    EXPECT_EQ(events.trades[1].price, px(100.01));
    EXPECT_EQ(events.trades[2].price, px(100.02));
    EXPECT_EQ(events.traded_volume(), 30);
    // 5 left over rests at the limit, best ask is now the untouched 100.03 level.
    EXPECT_EQ(book().best_bid(), px(100.02));
    EXPECT_EQ(book().best_ask(), px(100.03));
    expect_consistent();
}

TEST_F(OrderBookTest, TimePriorityWithinLevel) {
    const OrderId first = sell(px(100.00), 10);
    const OrderId second = sell(px(100.00), 10);
    const OrderId third = sell(px(100.00), 10);
    buy(px(100.00), 25);

    ASSERT_EQ(events.trades.size(), 3U);
    EXPECT_EQ(events.trades[0].resting_id, first);
    EXPECT_EQ(events.trades[1].resting_id, second);
    EXPECT_EQ(events.trades[2].resting_id, third);
    EXPECT_EQ(events.trades[2].quantity, 5);
    EXPECT_EQ(book().find(third)->leaves(), 5);
    EXPECT_FALSE(book().contains(first));
    expect_consistent();
}

TEST_F(OrderBookTest, SellAggressorHitsHighestBidFirst) {
    buy(px(99.00), 10);
    const OrderId best = buy(px(99.50), 10);
    limit(Side::Sell, px(98.00), 15);
    ASSERT_EQ(events.trades.size(), 2U);
    EXPECT_EQ(events.trades[0].resting_id, best);
    EXPECT_EQ(events.trades[0].price, px(99.50));
    EXPECT_EQ(events.trades[1].price, px(99.00));
    EXPECT_EQ(events.trades[0].aggressor_side, Side::Sell);
    expect_consistent();
}

TEST_F(OrderBookTest, NonCrossingOrdersDoNotTrade) {
    buy(px(99.99), 10);
    sell(px(100.00), 10);
    EXPECT_TRUE(events.trades.empty());
    EXPECT_EQ(book().order_count(), 2U);
}

TEST_F(OrderBookTest, CancelRemovesOrderAndReportsLeaves) {
    const OrderId id = buy(px(99.00), 100);
    events.clear();
    cancel(id);
    ASSERT_EQ(events.cancelled.size(), 1U);
    EXPECT_EQ(events.cancelled[0].order_id, id);
    EXPECT_EQ(events.cancelled[0].cancelled_quantity, 100);
    EXPECT_EQ(events.cancelled[0].reason, CancelReason::UserRequested);
    EXPECT_EQ(events.sequence, "dC");
    EXPECT_FALSE(book().contains(id));
    EXPECT_FALSE(book().best_bid().has_value());
    expect_consistent();
}

TEST_F(OrderBookTest, CancelAfterPartialFillReportsRemainingOnly) {
    const OrderId id = buy(px(100.00), 100);
    sell(px(100.00), 40);
    events.clear();
    cancel(id);
    ASSERT_EQ(events.cancelled.size(), 1U);
    EXPECT_EQ(events.cancelled[0].cancelled_quantity, 60);
}

TEST_F(OrderBookTest, CancelUnknownOrderIsRejected) {
    cancel(12345);
    ASSERT_EQ(events.rejected.size(), 1U);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::UnknownOrder);
    EXPECT_EQ(events.rejected[0].request, RequestKind::Cancel);
    EXPECT_EQ(events.rejected[0].order_id, 12345U);
}

TEST_F(OrderBookTest, DoubleCancelIsRejected) {
    const OrderId id = buy(px(100.00), 1);
    cancel(id);
    cancel(id);
    EXPECT_EQ(events.cancelled.size(), 1U);
    ASSERT_EQ(events.rejected.size(), 1U);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::UnknownOrder);
}

TEST_F(OrderBookTest, CancelMiddleOfQueuePreservesOthersPriority) {
    const OrderId a = sell(px(100.00), 10);
    const OrderId b = sell(px(100.00), 10);
    const OrderId c = sell(px(100.00), 10);
    cancel(b);
    buy(px(100.00), 20);
    ASSERT_EQ(events.trades.size(), 2U);
    EXPECT_EQ(events.trades[0].resting_id, a);
    EXPECT_EQ(events.trades[1].resting_id, c);
    expect_consistent();
}

TEST_F(OrderBookTest, DepthAggregatesLevels) {
    buy(px(99.00), 10);
    buy(px(99.00), 15);
    buy(px(98.00), 5);
    buy(px(97.00), 1);
    const auto depth = book().depth(Side::Buy, 2);
    ASSERT_EQ(depth.size(), 2U);
    EXPECT_EQ(depth[0], (LevelSnapshot{px(99.00), 25, 25, 2}));
    EXPECT_EQ(depth[1], (LevelSnapshot{px(98.00), 5, 5, 1}));
    EXPECT_EQ(book().depth(Side::Buy, 10).size(), 3U);
}

TEST_F(OrderBookTest, ForEachOrderVisitsInPriority) {
    const OrderId a = buy(px(99.00), 1);
    const OrderId b = buy(px(100.00), 1);
    const OrderId c = buy(px(99.00), 1);
    std::vector<OrderId> seen;
    book().for_each_order(Side::Buy, [&](const Order& o) { seen.push_back(o.id); });
    EXPECT_EQ(seen, (std::vector<OrderId>{b, a, c}));
}

TEST_F(OrderBookTest, EventOrderingForAggressiveOrderThatRests) {
    sell(px(100.00), 10);
    events.clear();
    buy(px(100.00), 15);
    // accept, trade, book execute of the resting order, then the remainder rests.
    EXPECT_EQ(events.sequence, "ATea");
    ASSERT_EQ(events.book_adds.size(), 1U);
    EXPECT_EQ(events.book_adds[0].quantity, 5);
}

TEST_F(OrderBookTest, MatchIdsAreSequential) {
    sell(px(100.00), 1);
    sell(px(100.00), 1);
    sell(px(100.00), 1);
    buy(px(100.00), 3);
    ASSERT_EQ(events.trades.size(), 3U);
    EXPECT_EQ(events.trades[0].match_id, 1U);
    EXPECT_EQ(events.trades[1].match_id, 2U);
    EXPECT_EQ(events.trades[2].match_id, 3U);
    EXPECT_EQ(events.book_executes[2].match_id, 3U);
}

TEST_F(OrderBookTest, TimestampsComeFromTheClock) {
    clock.set(5'000);
    buy(px(100.00), 1);
    EXPECT_EQ(events.accepted[0].timestamp, 5'000U);
    EXPECT_EQ(events.book_adds[0].timestamp, 5'000U);
    EXPECT_EQ(book().find(events.accepted[0].order_id)->entry_time, 5'000U);
}

// ---------------------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------------------

TEST_F(OrderBookTest, RejectsNonPositiveQuantity) {
    EXPECT_EQ(buy(px(100.00), 0), kInvalidOrderId);
    EXPECT_EQ(buy(px(100.00), -5), kInvalidOrderId);
    ASSERT_EQ(events.rejected.size(), 2U);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::InvalidQuantity);
    EXPECT_TRUE(events.accepted.empty());
}

TEST_F(OrderBookTest, RejectsQuantityAboveMaximum) {
    EXPECT_EQ(buy(px(100.00), 1'000'001), kInvalidOrderId);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::InvalidQuantity);
}

TEST_F(OrderBookTest, RejectsBadPrices) {
    EXPECT_EQ(buy(0, 1), kInvalidOrderId);
    EXPECT_EQ(buy(-100, 1), kInvalidOrderId);
    EXPECT_EQ(buy(kMaxPrice + 100, 1), kInvalidOrderId);
    EXPECT_EQ(buy(px(100.005), 1), kInvalidOrderId);
    ASSERT_EQ(events.rejected.size(), 4U);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::InvalidPrice);
    EXPECT_EQ(events.rejected[1].reason, RejectReason::InvalidPrice);
    EXPECT_EQ(events.rejected[2].reason, RejectReason::InvalidPrice);
    EXPECT_EQ(events.rejected[3].reason, RejectReason::PriceNotOnTick);
}

TEST_F(OrderBookTest, RejectedOrdersDoNotConsumeIds) {
    const OrderId first = buy(px(100.00), 1);
    buy(px(100.00), 0);
    const OrderId second = buy(px(100.00), 1);
    EXPECT_EQ(second, first + 1);
}

class LotSizeTest : public ::testing::Test {
  protected:
    LotSizeTest() : engine(events, clock) {
        InstrumentSpec spec;
        spec.symbol = "LOT";
        spec.tick_size = 500;
        spec.lot_size = 100;
        id = engine.add_instrument(spec);
    }
    RecordingListener events;
    ManualClock clock;
    MatchingEngine engine;
    InstrumentId id = 0;
};

TEST_F(LotSizeTest, EnforcesLotAndTick) {
    NewOrderRequest request;
    request.instrument = id;
    request.price = px(10.05);
    request.quantity = 150;
    EXPECT_EQ(engine.submit(request), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::QuantityNotOnLot);

    request.quantity = 200;
    request.price = px(10.01);
    EXPECT_EQ(engine.submit(request), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::PriceNotOnTick);

    request.price = px(10.05);
    EXPECT_NE(engine.submit(request), kInvalidOrderId);
}

}  // namespace
}  // namespace quill::test
