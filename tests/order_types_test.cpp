#include <gtest/gtest.h>

#include "test_support.hpp"

namespace quill::test {
namespace {

class OrderTypesTest : public EngineFixture {
  protected:
    OrderId post_only(Side side, Price price, Quantity quantity) {
        NewOrderRequest request;
        request.instrument = instrument;
        request.side = side;
        request.price = price;
        request.quantity = quantity;
        request.post_only = true;
        return engine.submit(request);
    }
};

// ---------------------------------------------------------------------------------------
// Market orders
// ---------------------------------------------------------------------------------------

TEST_F(OrderTypesTest, MarketOrderSweepsWithoutPriceLimit) {
    sell(px(100.00), 10);
    sell(px(150.00), 10);
    const OrderId id = market(Side::Buy, 20);
    ASSERT_NE(id, kInvalidOrderId);
    ASSERT_EQ(events.trades.size(), 2U);
    EXPECT_EQ(events.trades[1].price, px(150.00));
    EXPECT_FALSE(book().best_ask().has_value());
    EXPECT_TRUE(events.cancelled.empty());
    expect_consistent();
}

TEST_F(OrderTypesTest, MarketOrderRemainderIsCancelled) {
    sell(px(100.00), 10);
    const OrderId id = market(Side::Buy, 25);
    ASSERT_EQ(events.cancelled.size(), 1U);
    EXPECT_EQ(events.cancelled[0].order_id, id);
    EXPECT_EQ(events.cancelled[0].cancelled_quantity, 15);
    EXPECT_EQ(events.cancelled[0].reason, CancelReason::MarketOrderRemainder);
    EXPECT_FALSE(book().best_bid().has_value());  // never rests
    expect_consistent();
}

TEST_F(OrderTypesTest, MarketOrderIntoEmptyBookIsRejected) {
    EXPECT_EQ(market(Side::Sell, 10), kInvalidOrderId);
    ASSERT_EQ(events.rejected.size(), 1U);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::NoLiquidity);
}

TEST_F(OrderTypesTest, MarketOrderMustNotBeGoodTillCancel) {
    sell(px(100.00), 10);
    EXPECT_EQ(market(Side::Buy, 10, TimeInForce::GoodTillCancel), kInvalidOrderId);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::InvalidTimeInForce);
}

TEST_F(OrderTypesTest, MarketFillOrKill) {
    sell(px(100.00), 10);
    EXPECT_EQ(market(Side::Buy, 11, TimeInForce::FillOrKill), kInvalidOrderId);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::FillOrKillUnfillable);
    EXPECT_NE(market(Side::Buy, 10, TimeInForce::FillOrKill), kInvalidOrderId);
    EXPECT_EQ(events.traded_volume(), 10);
}

// ---------------------------------------------------------------------------------------
// Immediate-or-cancel
// ---------------------------------------------------------------------------------------

TEST_F(OrderTypesTest, ImmediateOrCancelFillsThenCancels) {
    sell(px(100.00), 10);
    sell(px(101.00), 10);
    const OrderId id = limit(Side::Buy, px(100.00), 25, 0, TimeInForce::ImmediateOrCancel);
    EXPECT_EQ(events.traded_volume(), 10);
    ASSERT_EQ(events.cancelled.size(), 1U);
    EXPECT_EQ(events.cancelled[0].order_id, id);
    EXPECT_EQ(events.cancelled[0].cancelled_quantity, 15);
    EXPECT_EQ(events.cancelled[0].reason, CancelReason::ImmediateOrCancelRemainder);
    EXPECT_FALSE(book().best_bid().has_value());
    EXPECT_EQ(book().best_ask(), px(101.00));
    expect_consistent();
}

TEST_F(OrderTypesTest, ImmediateOrCancelWithNothingToHitCancelsEverything) {
    const OrderId id = limit(Side::Buy, px(100.00), 5, 0, TimeInForce::ImmediateOrCancel);
    EXPECT_NE(id, kInvalidOrderId);
    EXPECT_EQ(events.sequence, "AC");
    EXPECT_EQ(events.cancelled[0].cancelled_quantity, 5);
}

TEST_F(OrderTypesTest, FullyFilledImmediateOrCancelHasNoCancel) {
    sell(px(100.00), 10);
    limit(Side::Buy, px(100.00), 10, 0, TimeInForce::ImmediateOrCancel);
    EXPECT_TRUE(events.cancelled.empty());
}

// ---------------------------------------------------------------------------------------
// Fill-or-kill
// ---------------------------------------------------------------------------------------

TEST_F(OrderTypesTest, FillOrKillRejectedWhenInsufficient) {
    sell(px(100.00), 10);
    sell(px(101.00), 10);
    EXPECT_EQ(limit(Side::Buy, px(100.00), 15, 0, TimeInForce::FillOrKill), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::FillOrKillUnfillable);
    EXPECT_TRUE(events.trades.empty());
    EXPECT_EQ(book().order_count(), 2U);  // book untouched
}

TEST_F(OrderTypesTest, FillOrKillExecutesAcrossLevels) {
    sell(px(100.00), 10);
    sell(px(101.00), 10);
    EXPECT_NE(limit(Side::Buy, px(101.00), 15, 0, TimeInForce::FillOrKill), kInvalidOrderId);
    EXPECT_EQ(events.traded_volume(), 15);
    EXPECT_TRUE(events.cancelled.empty());
    expect_consistent();
}

TEST_F(OrderTypesTest, FillOrKillCountsIcebergReserve) {
    iceberg(Side::Sell, px(100.00), 100, 10);
    EXPECT_NE(limit(Side::Buy, px(100.00), 100, 0, TimeInForce::FillOrKill), kInvalidOrderId);
    EXPECT_EQ(events.traded_volume(), 100);
    EXPECT_EQ(book().order_count(), 0U);
}

TEST_F(OrderTypesTest, FillOrKillExactQuantityAtLimit) {
    sell(px(100.00), 7);
    sell(px(100.00), 3);
    EXPECT_NE(limit(Side::Buy, px(100.00), 10, 0, TimeInForce::FillOrKill), kInvalidOrderId);
    EXPECT_EQ(book().order_count(), 0U);
}

// ---------------------------------------------------------------------------------------
// Post-only
// ---------------------------------------------------------------------------------------

TEST_F(OrderTypesTest, PostOnlyRestsWhenPassive) {
    sell(px(101.00), 10);
    const OrderId id = post_only(Side::Buy, px(100.00), 10);
    EXPECT_NE(id, kInvalidOrderId);
    EXPECT_TRUE(book().contains(id));
    EXPECT_TRUE(book().find(id)->post_only);
}

TEST_F(OrderTypesTest, PostOnlyRejectedWhenItWouldCross) {
    sell(px(100.00), 10);
    EXPECT_EQ(post_only(Side::Buy, px(100.00), 10), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::PostOnlyWouldCross);
    EXPECT_TRUE(events.trades.empty());
    buy(px(99.00), 1);
    EXPECT_EQ(post_only(Side::Sell, px(98.00), 10), kInvalidOrderId);
}

TEST_F(OrderTypesTest, PostOnlyRequiresGoodTillCancel) {
    NewOrderRequest request;
    request.instrument = instrument;
    request.price = px(100.00);
    request.quantity = 1;
    request.post_only = true;
    request.tif = TimeInForce::ImmediateOrCancel;
    EXPECT_EQ(engine.submit(request), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::InvalidTimeInForce);
}

// ---------------------------------------------------------------------------------------
// Iceberg
// ---------------------------------------------------------------------------------------

TEST_F(OrderTypesTest, IcebergDisplaysOnlyPeak) {
    const OrderId id = iceberg(Side::Sell, px(100.00), 1000, 100);
    const auto depth = book().depth(Side::Sell, 1);
    ASSERT_EQ(depth.size(), 1U);
    EXPECT_EQ(depth[0].visible, 100);
    EXPECT_EQ(depth[0].total, 1000);
    ASSERT_EQ(events.book_adds.size(), 1U);
    EXPECT_EQ(events.book_adds[0].quantity, 100);
    EXPECT_EQ(book().find(id)->hidden, 900);
}

TEST_F(OrderTypesTest, IcebergRefillLosesTimePriority) {
    const OrderId berg = iceberg(Side::Sell, px(100.00), 300, 100);
    const OrderId plain = sell(px(100.00), 50);
    events.clear();

    buy(px(100.00), 120);
    // First 100 from the iceberg's peak, then the refill goes behind `plain`.
    ASSERT_EQ(events.trades.size(), 2U);
    EXPECT_EQ(events.trades[0].resting_id, berg);
    EXPECT_EQ(events.trades[0].quantity, 100);
    EXPECT_EQ(events.trades[1].resting_id, plain);
    EXPECT_EQ(events.trades[1].quantity, 20);
    // Refill published as a fresh add of the next tranche.
    ASSERT_EQ(events.book_adds.size(), 1U);
    EXPECT_EQ(events.book_adds[0].order_id, berg);
    EXPECT_EQ(events.book_adds[0].quantity, 100);

    std::vector<OrderId> queue;
    book().for_each_order(Side::Sell, [&](const Order& o) { queue.push_back(o.id); });
    EXPECT_EQ(queue, (std::vector<OrderId>{plain, berg}));
    expect_consistent();
}

TEST_F(OrderTypesTest, IcebergFullyConsumedByOneLargeAggressor) {
    const OrderId berg = iceberg(Side::Buy, px(50.00), 250, 100);
    limit(Side::Sell, px(50.00), 400);
    // 100 + 100 + 50, then the rest of the sell rests.
    ASSERT_EQ(events.trades.size(), 3U);
    EXPECT_EQ(events.trades[2].quantity, 50);
    EXPECT_EQ(events.trades[2].resting_leaves, 0);
    EXPECT_FALSE(book().contains(berg));
    EXPECT_EQ(book().depth(Side::Sell, 1)[0].visible, 150);
    expect_consistent();
}

TEST_F(OrderTypesTest, AggressiveIcebergRestsWithPeakDisplay) {
    sell(px(100.00), 150);
    const OrderId berg = iceberg(Side::Buy, px(100.00), 1000, 200);
    EXPECT_EQ(events.traded_volume(), 150);
    const Order* order = book().find(berg);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->visible, 200);
    EXPECT_EQ(order->hidden, 650);
    expect_consistent();
}

TEST_F(OrderTypesTest, IcebergValidation) {
    EXPECT_EQ(iceberg(Side::Buy, px(100.00), 100, 100), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::InvalidDisplayQuantity);
    EXPECT_EQ(iceberg(Side::Buy, px(100.00), 100, -1), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::InvalidDisplayQuantity);

    NewOrderRequest request;
    request.instrument = instrument;
    request.price = px(100.00);
    request.quantity = 100;
    request.display_quantity = 10;
    request.tif = TimeInForce::ImmediateOrCancel;
    EXPECT_EQ(engine.submit(request), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::InvalidTimeInForce);
}

// ---------------------------------------------------------------------------------------
// Self-trade prevention
// ---------------------------------------------------------------------------------------

class SelfTradeTest : public EngineFixture {
  protected:
    OrderId submit_stp(Side side, Price price, Quantity quantity, ParticipantId who,
                       SelfTradePrevention mode, TimeInForce tif = TimeInForce::GoodTillCancel) {
        NewOrderRequest request;
        request.instrument = instrument;
        request.side = side;
        request.price = price;
        request.quantity = quantity;
        request.participant = who;
        request.stp = mode;
        request.tif = tif;
        return engine.submit(request);
    }
};

TEST_F(SelfTradeTest, NoPreventionAllowsSelfMatch) {
    limit(Side::Sell, px(100.00), 10, 7);
    submit_stp(Side::Buy, px(100.00), 10, 7, SelfTradePrevention::None);
    EXPECT_EQ(events.traded_volume(), 10);
}

TEST_F(SelfTradeTest, CancelRestingSkipsOwnOrders) {
    const OrderId own = limit(Side::Sell, px(100.00), 10, 7);
    const OrderId other = limit(Side::Sell, px(100.00), 10, 8);
    submit_stp(Side::Buy, px(100.00), 10, 7, SelfTradePrevention::CancelResting);

    ASSERT_EQ(events.trades.size(), 1U);
    EXPECT_EQ(events.trades[0].resting_id, other);
    ASSERT_EQ(events.cancelled.size(), 1U);
    EXPECT_EQ(events.cancelled[0].order_id, own);
    EXPECT_EQ(events.cancelled[0].reason, CancelReason::SelfTradePrevention);
    EXPECT_EQ(book().order_count(), 0U);
    expect_consistent();
}

TEST_F(SelfTradeTest, CancelIncomingStopsAtOwnOrder) {
    const OrderId other = limit(Side::Sell, px(100.00), 5, 8);
    const OrderId own = limit(Side::Sell, px(100.00), 10, 7);
    const OrderId incoming =
        submit_stp(Side::Buy, px(100.00), 20, 7, SelfTradePrevention::CancelIncoming);

    ASSERT_EQ(events.trades.size(), 1U);
    EXPECT_EQ(events.trades[0].resting_id, other);
    ASSERT_EQ(events.cancelled.size(), 1U);
    EXPECT_EQ(events.cancelled[0].order_id, incoming);
    EXPECT_EQ(events.cancelled[0].cancelled_quantity, 15);
    EXPECT_TRUE(book().contains(own));
    EXPECT_FALSE(book().contains(incoming));
    expect_consistent();
}

TEST_F(SelfTradeTest, CancelBothRemovesBoth) {
    const OrderId own = limit(Side::Sell, px(100.00), 10, 7);
    const OrderId incoming =
        submit_stp(Side::Buy, px(100.00), 4, 7, SelfTradePrevention::CancelBoth);
    EXPECT_TRUE(events.trades.empty());
    ASSERT_EQ(events.cancelled.size(), 2U);
    EXPECT_EQ(events.cancelled[0].order_id, own);
    EXPECT_EQ(events.cancelled[1].order_id, incoming);
    EXPECT_EQ(book().order_count(), 0U);
}

TEST_F(SelfTradeTest, ParticipantZeroIsAnonymousAndNeverPrevented) {
    limit(Side::Sell, px(100.00), 10, 0);
    submit_stp(Side::Buy, px(100.00), 10, 0, SelfTradePrevention::CancelIncoming);
    EXPECT_EQ(events.traded_volume(), 10);
}

TEST_F(SelfTradeTest, FillOrKillBlockedByOwnOrderAhead) {
    limit(Side::Sell, px(100.00), 5, 8);
    limit(Side::Sell, px(100.00), 10, 7);
    limit(Side::Sell, px(100.00), 50, 9);
    EXPECT_EQ(submit_stp(Side::Buy, px(100.00), 20, 7, SelfTradePrevention::CancelIncoming,
                         TimeInForce::FillOrKill),
              kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::FillOrKillUnfillable);
    EXPECT_NE(submit_stp(Side::Buy, px(100.00), 5, 7, SelfTradePrevention::CancelIncoming,
                         TimeInForce::FillOrKill),
              kInvalidOrderId);
}

TEST_F(SelfTradeTest, FillOrKillWithCancelRestingIgnoresOwnLiquidity) {
    limit(Side::Sell, px(100.00), 10, 7);
    limit(Side::Sell, px(100.00), 10, 8);
    EXPECT_EQ(submit_stp(Side::Buy, px(100.00), 15, 7, SelfTradePrevention::CancelResting,
                         TimeInForce::FillOrKill),
              kInvalidOrderId);
    EXPECT_NE(submit_stp(Side::Buy, px(100.00), 10, 7, SelfTradePrevention::CancelResting,
                         TimeInForce::FillOrKill),
              kInvalidOrderId);
    EXPECT_EQ(events.traded_volume(), 10);
    EXPECT_EQ(book().order_count(), 0U);
}

// ---------------------------------------------------------------------------------------
// Price collar
// ---------------------------------------------------------------------------------------

class CollarTest : public ::testing::Test {
  protected:
    CollarTest() : engine(events, clock) {
        InstrumentSpec spec;
        spec.symbol = "BAND";
        spec.tick_size = 100;
        spec.collar_bps = 500;  // 5%
        id = engine.add_instrument(spec);
    }

    OrderId order(Side side, OrderType type, Price price, Quantity quantity,
                  TimeInForce tif = TimeInForce::GoodTillCancel) {
        NewOrderRequest request;
        request.instrument = id;
        request.side = side;
        request.type = type;
        request.price = price;
        request.quantity = quantity;
        request.tif = tif;
        return engine.submit(request);
    }

    RecordingListener events;
    ManualClock clock;
    MatchingEngine engine;
    InstrumentId id = 0;
};

TEST_F(CollarTest, InactiveBeforeFirstTrade) {
    EXPECT_NE(order(Side::Buy, OrderType::Limit, px(1.00), 1), kInvalidOrderId);
    EXPECT_NE(order(Side::Sell, OrderType::Limit, px(1000.00), 1), kInvalidOrderId);
}

TEST_F(CollarTest, RejectsLimitOrdersOutsideBand) {
    order(Side::Sell, OrderType::Limit, px(100.00), 1);
    order(Side::Buy, OrderType::Limit, px(100.00), 1);  // prints at 100
    EXPECT_EQ(order(Side::Buy, OrderType::Limit, px(94.00), 1), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::PriceOutsideCollar);
    EXPECT_EQ(order(Side::Sell, OrderType::Limit, px(105.01), 1), kInvalidOrderId);
    EXPECT_NE(order(Side::Buy, OrderType::Limit, px(95.00), 1), kInvalidOrderId);
    EXPECT_NE(order(Side::Sell, OrderType::Limit, px(105.00), 1), kInvalidOrderId);
}

TEST_F(CollarTest, MarketOrdersStopAtBand) {
    order(Side::Sell, OrderType::Limit, px(100.00), 1);
    order(Side::Buy, OrderType::Limit, px(100.00), 1);   // last = 100, band [95, 105]
    order(Side::Sell, OrderType::Limit, px(105.00), 5);  // at the edge of the band
    order(Side::Sell, OrderType::Limit, px(98.00), 1);
    order(Side::Buy, OrderType::Limit, px(98.00), 1);  // last = 98, band [93.10, 102.90]
    order(Side::Sell, OrderType::Limit, px(101.00), 5);

    const auto before = events.trades.size();
    order(Side::Buy, OrderType::Market, 0, 20, TimeInForce::ImmediateOrCancel);
    // The 105 offer is now outside the band, so the market order stops after 101.
    ASSERT_EQ(events.trades.size() - before, 1U);
    EXPECT_EQ(events.trades.back().price, px(101.00));
    EXPECT_EQ(events.cancelled.back().cancelled_quantity, 15);
    EXPECT_EQ(events.cancelled.back().reason, CancelReason::MarketOrderRemainder);
    ASSERT_TRUE(engine.book(id)->best_ask().has_value());
    EXPECT_EQ(*engine.book(id)->best_ask(), px(105.00));
}

TEST_F(CollarTest, UnchangedPriceReplaceAllowedOutsideBand) {
    const OrderId stale = order(Side::Sell, OrderType::Limit, px(105.00), 10);
    order(Side::Sell, OrderType::Limit, px(100.00), 1);
    order(Side::Buy, OrderType::Limit, px(100.00), 1);  // last = 100
    order(Side::Sell, OrderType::Limit, px(96.00), 1);
    order(Side::Buy, OrderType::Limit, px(96.00), 1);  // last = 96, 105 now outside
    engine.replace(ReplaceRequest{id, stale, 0, px(105.00), 5});
    ASSERT_EQ(events.replaced.size(), 1U);
    EXPECT_TRUE(events.replaced[0].priority_kept);
    engine.replace(ReplaceRequest{id, stale, 0, px(106.00), 5});
    EXPECT_EQ(events.rejected.back().reason, RejectReason::PriceOutsideCollar);
}

}  // namespace
}  // namespace quill::test
