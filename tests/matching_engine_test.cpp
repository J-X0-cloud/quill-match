#include "quill/matching_engine.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "test_support.hpp"

namespace quill::test {
namespace {

class MatchingEngineTest : public ::testing::Test {
  protected:
    MatchingEngineTest() : engine(events, clock) {}

    InstrumentId add(const std::string& symbol) {
        InstrumentSpec spec;
        spec.symbol = symbol;
        return engine.add_instrument(spec);
    }

    static NewOrderRequest order(InstrumentId instrument, Side side, Price price,
                                 Quantity quantity) {
        NewOrderRequest request;
        request.instrument = instrument;
        request.side = side;
        request.price = price;
        request.quantity = quantity;
        return request;
    }

    RecordingListener events;
    ManualClock clock;
    MatchingEngine engine;
};

TEST_F(MatchingEngineTest, InstrumentIdsAreDenseFromOne) {
    EXPECT_EQ(add("AAA"), 1U);
    EXPECT_EQ(add("BBB"), 2U);
    EXPECT_EQ(engine.instrument_count(), 2U);
    EXPECT_EQ(engine.find_instrument("BBB"), 2U);
    EXPECT_FALSE(engine.find_instrument("CCC").has_value());
    EXPECT_EQ(engine.book(0), nullptr);
    EXPECT_EQ(engine.book(3), nullptr);
    EXPECT_EQ(engine.book(1)->spec().symbol, "AAA");
}

TEST_F(MatchingEngineTest, RejectsBadInstrumentDefinitions) {
    add("AAA");
    EXPECT_THROW(add("AAA"), std::invalid_argument);
    EXPECT_THROW(add(""), std::invalid_argument);
    InstrumentSpec spec;
    spec.symbol = "ZERO";
    spec.tick_size = 0;
    EXPECT_THROW(engine.add_instrument(spec), std::invalid_argument);
}

TEST_F(MatchingEngineTest, BooksAreIndependent) {
    const InstrumentId a = add("AAA");
    const InstrumentId b = add("BBB");
    engine.submit(order(a, Side::Sell, px(10.00), 5));
    engine.submit(order(b, Side::Buy, px(10.00), 5));
    EXPECT_TRUE(events.trades.empty());
    EXPECT_EQ(engine.book(a)->order_count(), 1U);
    EXPECT_EQ(engine.book(b)->order_count(), 1U);

    engine.submit(order(a, Side::Buy, px(10.00), 5));
    ASSERT_EQ(events.trades.size(), 1U);
    EXPECT_EQ(events.trades[0].instrument, a);
}

TEST_F(MatchingEngineTest, OrderIdsAreGlobalAcrossInstruments) {
    const InstrumentId a = add("AAA");
    const InstrumentId b = add("BBB");
    EXPECT_EQ(engine.submit(order(a, Side::Buy, px(1.00), 1)), 1U);
    EXPECT_EQ(engine.submit(order(b, Side::Buy, px(1.00), 1)), 2U);
    EXPECT_EQ(engine.submit(order(a, Side::Buy, px(1.00), 1)), 3U);
}

TEST_F(MatchingEngineTest, MatchIdsAreGlobalAcrossInstruments) {
    const InstrumentId a = add("AAA");
    const InstrumentId b = add("BBB");
    engine.submit(order(a, Side::Sell, px(1.00), 1));
    engine.submit(order(a, Side::Buy, px(1.00), 1));
    engine.submit(order(b, Side::Sell, px(1.00), 1));
    engine.submit(order(b, Side::Buy, px(1.00), 1));
    ASSERT_EQ(events.trades.size(), 2U);
    EXPECT_EQ(events.trades[0].match_id, 1U);
    EXPECT_EQ(events.trades[1].match_id, 2U);
    EXPECT_EQ(engine.last_match_id(), 2U);
}

TEST_F(MatchingEngineTest, UnknownInstrumentRejectedForEveryRequestKind) {
    engine.submit(order(42, Side::Buy, px(1.00), 1));
    engine.cancel(CancelRequest{42, 7, 99});
    engine.replace(ReplaceRequest{42, 7, 100, px(1.00), 1});
    ASSERT_EQ(events.rejected.size(), 3U);
    for (const auto& r : events.rejected) {
        EXPECT_EQ(r.reason, RejectReason::UnknownInstrument);
        EXPECT_EQ(r.instrument, 42U);
    }
    EXPECT_EQ(events.rejected[0].request, RequestKind::NewOrder);
    EXPECT_EQ(events.rejected[1].request, RequestKind::Cancel);
    EXPECT_EQ(events.rejected[1].client_tag, 99U);
    EXPECT_EQ(events.rejected[2].request, RequestKind::Replace);
    EXPECT_EQ(engine.commands_processed(), 3U);
}

TEST_F(MatchingEngineTest, ClientAssignedIdsAreHonouredAndAdvanceSequence) {
    const InstrumentId a = add("AAA");
    auto request = order(a, Side::Buy, px(1.00), 1);
    request.order_id = 500;
    EXPECT_EQ(engine.submit(request), 500U);
    EXPECT_EQ(engine.submit(order(a, Side::Buy, px(1.00), 1)), 501U);
    // Reusing a live id is refused.
    EXPECT_EQ(engine.submit(request), kInvalidOrderId);
    EXPECT_EQ(events.rejected.back().reason, RejectReason::DuplicateOrderId);
}

TEST_F(MatchingEngineTest, ProcessDispatchesCommandVariants) {
    const InstrumentId a = add("AAA");
    engine.process(Command{order(a, Side::Buy, px(5.00), 10), 0});
    ASSERT_EQ(events.accepted.size(), 1U);
    const OrderId id = events.accepted[0].order_id;
    engine.process(Command{ReplaceRequest{a, id, 1, px(5.00), 4}, 0});
    ASSERT_EQ(events.replaced.size(), 1U);
    engine.process(Command{CancelRequest{a, id, 2}, 0});
    ASSERT_EQ(events.cancelled.size(), 1U);
    EXPECT_EQ(events.cancelled[0].client_tag, 2U);
    EXPECT_EQ(events.cancelled[0].cancelled_quantity, 4);
    EXPECT_EQ(engine.commands_processed(), 3U);
}

TEST_F(MatchingEngineTest, AcceptedEventEchoesRequest) {
    const InstrumentId a = add("AAA");
    auto request = order(a, Side::Sell, px(12.34), 77);
    request.client_tag = 0xDEADBEEF;
    request.participant = 9;
    engine.submit(request);
    ASSERT_EQ(events.accepted.size(), 1U);
    const OrderAccepted& e = events.accepted[0];
    EXPECT_EQ(e.client_tag, 0xDEADBEEFU);
    EXPECT_EQ(e.participant, 9U);
    EXPECT_EQ(e.side, Side::Sell);
    EXPECT_EQ(e.price, px(12.34));
    EXPECT_EQ(e.quantity, 77);
    EXPECT_EQ(e.instrument, a);
}

TEST(EventFanout, ForwardsToAllListenersInOrder) {
    RecordingListener first;
    RecordingListener second;
    EventFanout fanout;
    fanout.add(first);
    fanout.add(second);
    ManualClock clock;
    MatchingEngine engine(fanout, clock);
    InstrumentSpec spec;
    spec.symbol = "X";
    const InstrumentId id = engine.add_instrument(spec);
    NewOrderRequest request;
    request.instrument = id;
    request.price = px(1.00);
    request.quantity = 1;
    engine.submit(request);
    EXPECT_EQ(first.sequence, "Aa");
    EXPECT_EQ(second.sequence, "Aa");
}

TEST(ManualClock, StepsOnEveryRead) {
    ManualClock clock(10, 5);
    EXPECT_EQ(clock.now(), 10U);
    EXPECT_EQ(clock.now(), 15U);
    clock.advance(100);
    EXPECT_EQ(clock.now(), 120U);
    clock.set(1);
    EXPECT_EQ(clock.now(), 1U);
}

TEST(SteadyClock, IsMonotonic) {
    SteadyClock clock;
    const Timestamp a = clock.now();
    const Timestamp b = clock.now();
    EXPECT_LE(a, b);
}

}  // namespace
}  // namespace quill::test
