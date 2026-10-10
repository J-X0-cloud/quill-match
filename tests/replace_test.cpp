#include <gtest/gtest.h>

#include "test_support.hpp"

namespace quill::test {
namespace {

using ReplaceTest = EngineFixture;

std::vector<OrderId> queue_at(const OrderBook& book, Side side) {
    std::vector<OrderId> ids;
    book.for_each_order(side, [&](const Order& o) { ids.push_back(o.id); });
    return ids;
}

TEST_F(ReplaceTest, SizeDownKeepsPriority) {
    const OrderId a = buy(px(100.00), 100);
    const OrderId b = buy(px(100.00), 100);
    events.clear();
    replace(a, px(100.00), 40);

    ASSERT_EQ(events.replaced.size(), 1U);
    EXPECT_TRUE(events.replaced[0].priority_kept);
    EXPECT_EQ(events.replaced[0].leaves, 40);
    EXPECT_EQ(events.sequence, "Ux");
    ASSERT_EQ(events.book_reduces.size(), 1U);
    EXPECT_EQ(events.book_reduces[0].quantity, 60);
    EXPECT_EQ(queue_at(book(), Side::Buy), (std::vector<OrderId>{a, b}));
    EXPECT_EQ(book().depth(Side::Buy, 1)[0].visible, 140);
    expect_consistent();
}

TEST_F(ReplaceTest, SizeUpLosesPriority) {
    const OrderId a = buy(px(100.00), 100);
    const OrderId b = buy(px(100.00), 100);
    events.clear();
    replace(a, px(100.00), 150);

    ASSERT_EQ(events.replaced.size(), 1U);
    EXPECT_FALSE(events.replaced[0].priority_kept);
    EXPECT_EQ(events.sequence, "dUa");
    EXPECT_EQ(queue_at(book(), Side::Buy), (std::vector<OrderId>{b, a}));
    EXPECT_EQ(book().find(a)->leaves(), 150);
    expect_consistent();
}

TEST_F(ReplaceTest, PriceChangeMovesLevel) {
    const OrderId a = buy(px(100.00), 10);
    replace(a, px(99.00), 10);
    EXPECT_EQ(book().best_bid(), px(99.00));
    EXPECT_EQ(book().ladder(Side::Buy).size(), 1U);
    EXPECT_EQ(book().find(a)->price, px(99.00));
    expect_consistent();
}

TEST_F(ReplaceTest, ReplaceIntoCrossTrades) {
    const OrderId ask = sell(px(101.00), 30);
    const OrderId bid = buy(px(100.00), 50);
    events.clear();
    replace(bid, px(101.00), 50);

    ASSERT_EQ(events.trades.size(), 1U);
    EXPECT_EQ(events.trades[0].aggressor_id, bid);
    EXPECT_EQ(events.trades[0].resting_id, ask);
    EXPECT_EQ(events.trades[0].quantity, 30);
    EXPECT_EQ(events.sequence, "dUTea");
    EXPECT_EQ(book().find(bid)->leaves(), 20);
    EXPECT_EQ(book().find(bid)->executed, 30);
    expect_consistent();
}

TEST_F(ReplaceTest, ReplaceThatFullyFillsRemovesOrder) {
    sell(px(101.00), 50);
    const OrderId bid = buy(px(100.00), 50);
    replace(bid, px(101.00), 50);
    EXPECT_FALSE(book().contains(bid));
    EXPECT_EQ(book().order_count(), 0U);
    expect_consistent();
}

TEST_F(ReplaceTest, QuantityCountsExecutedPortion) {
    const OrderId bid = buy(px(100.00), 100);
    sell(px(100.00), 60);  // 40 left
    events.clear();
    replace(bid, px(100.00), 80);  // total 80, so 20 leaves: a reduction
    ASSERT_EQ(events.replaced.size(), 1U);
    EXPECT_TRUE(events.replaced[0].priority_kept);
    EXPECT_EQ(book().find(bid)->leaves(), 20);
    EXPECT_EQ(book().find(bid)->quantity, 80);
    expect_consistent();
}

TEST_F(ReplaceTest, QuantityAtOrBelowExecutedIsRejected) {
    const OrderId bid = buy(px(100.00), 100);
    sell(px(100.00), 60);
    events.clear();
    replace(bid, px(100.00), 60);
    ASSERT_EQ(events.rejected.size(), 1U);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::QuantityBelowExecuted);
    EXPECT_EQ(events.rejected[0].request, RequestKind::Replace);
    EXPECT_EQ(book().find(bid)->leaves(), 40);
}

TEST_F(ReplaceTest, UnknownOrderIsRejected) {
    replace(999, px(100.00), 10);
    ASSERT_EQ(events.rejected.size(), 1U);
    EXPECT_EQ(events.rejected[0].reason, RejectReason::UnknownOrder);
}

TEST_F(ReplaceTest, InvalidPriceOrQuantityRejectedAndOrderUntouched) {
    const OrderId a = buy(px(100.00), 10);
    replace(a, px(100.001), 10);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::PriceNotOnTick);
    replace(a, px(100.00), 0);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::InvalidQuantity);
    EXPECT_EQ(book().find(a)->leaves(), 10);
    EXPECT_EQ(book().find(a)->price, px(100.00));
}

TEST_F(ReplaceTest, PostOnlyReplaceThatWouldCrossIsRejected) {
    sell(px(101.00), 10);
    NewOrderRequest request;
    request.instrument = instrument;
    request.side = Side::Buy;
    request.price = px(100.00);
    request.quantity = 10;
    request.post_only = true;
    const OrderId id = engine.submit(request);
    replace(id, px(101.00), 10);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::PostOnlyWouldCross);
    EXPECT_EQ(book().find(id)->price, px(100.00));
    EXPECT_TRUE(events.trades.empty());
}

TEST_F(ReplaceTest, IcebergSizeDownConsumesReserveFirst) {
    const OrderId berg = iceberg(Side::Sell, px(100.00), 500, 100);
    events.clear();
    replace(berg, px(100.00), 250);
    const Order* order = book().find(berg);
    EXPECT_EQ(order->visible, 100);
    EXPECT_EQ(order->hidden, 150);
    EXPECT_TRUE(events.book_reduces.empty());  // displayed size did not change

    replace(berg, px(100.00), 60);
    EXPECT_EQ(order->visible, 60);
    EXPECT_EQ(order->hidden, 0);
    ASSERT_EQ(events.book_reduces.size(), 1U);
    EXPECT_EQ(events.book_reduces[0].quantity, 40);
    expect_consistent();
}

TEST_F(ReplaceTest, IcebergReplacedBelowPeakDisplaysEverything) {
    const OrderId berg = iceberg(Side::Sell, px(100.00), 500, 100);
    replace(berg, px(101.00), 80);
    const Order* order = book().find(berg);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->visible, 80);
    EXPECT_EQ(order->hidden, 0);
    EXPECT_EQ(book().depth(Side::Sell, 1)[0].total, 80);
    expect_consistent();
}

TEST_F(ReplaceTest, IcebergRepriceRedisplaysPeak) {
    const OrderId berg = iceberg(Side::Sell, px(100.00), 500, 100);
    replace(berg, px(101.00), 400);
    const Order* order = book().find(berg);
    EXPECT_EQ(order->visible, 100);
    EXPECT_EQ(order->hidden, 300);
    EXPECT_EQ(events.book_adds.back().quantity, 100);
    expect_consistent();
}

TEST_F(ReplaceTest, SelfTradePreventionAppliesOnReplace) {
    limit(Side::Sell, px(101.00), 10, 5);
    NewOrderRequest request;
    request.instrument = instrument;
    request.side = Side::Buy;
    request.price = px(100.00);
    request.quantity = 10;
    request.participant = 5;
    request.stp = SelfTradePrevention::CancelIncoming;
    const OrderId bid = engine.submit(request);
    replace(bid, px(101.00), 10);
    EXPECT_TRUE(events.trades.empty());
    ASSERT_FALSE(events.cancelled.empty());
    EXPECT_EQ(events.cancelled.back().order_id, bid);
    EXPECT_EQ(events.cancelled.back().reason, CancelReason::SelfTradePrevention);
    EXPECT_FALSE(book().contains(bid));
    expect_consistent();
}

}  // namespace
}  // namespace quill::test
