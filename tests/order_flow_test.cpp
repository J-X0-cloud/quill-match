#include "quill/sim/order_flow.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <variant>

#include "test_support.hpp"

namespace quill::test {
namespace {

sim::FlowConfig config_for(std::vector<InstrumentId> ids, std::uint64_t seed) {
    sim::FlowConfig config;
    config.instruments = std::move(ids);
    config.seed = seed;
    return config;
}

TEST(OrderFlow, IsDeterministicForASeed) {
    sim::OrderFlowGenerator a(config_for({1, 2}, 5));
    sim::OrderFlowGenerator b(config_for({1, 2}, 5));
    for (int i = 0; i < 5'000; ++i) {
        const Command x = a.next();
        const Command y = b.next();
        ASSERT_EQ(x.payload.index(), y.payload.index());
        if (const auto* order = std::get_if<NewOrderRequest>(&x.payload)) {
            const auto& other = std::get<NewOrderRequest>(y.payload);
            ASSERT_EQ(order->order_id, other.order_id);
            ASSERT_EQ(order->price, other.price);
            ASSERT_EQ(order->quantity, other.quantity);
        }
    }
}

TEST(OrderFlow, ProducesValidMixOfCommands) {
    sim::FlowConfig config = config_for({1, 2, 3}, 9);
    sim::OrderFlowGenerator flow(config);
    std::size_t news = 0;
    std::size_t cancels = 0;
    std::size_t replaces = 0;
    std::size_t markets = 0;
    std::size_t icebergs = 0;
    for (const Command& c : flow.generate(50'000)) {
        if (const auto* order = std::get_if<NewOrderRequest>(&c.payload)) {
            ++news;
            EXPECT_GE(order->instrument, 1U);
            EXPECT_LE(order->instrument, 3U);
            EXPECT_GE(order->quantity, 1);
            EXPECT_LE(order->quantity, config.max_quantity);
            if (order->type == OrderType::Market) {
                ++markets;
                EXPECT_NE(order->tif, TimeInForce::GoodTillCancel);
            } else {
                EXPECT_EQ(order->price % config.tick_size, 0);
                EXPECT_GT(order->price, 0);
            }
            if (order->display_quantity > 0) {
                ++icebergs;
                EXPECT_LT(order->display_quantity, order->quantity);
            }
        } else if (std::holds_alternative<CancelRequest>(c.payload)) {
            ++cancels;
        } else {
            ++replaces;
        }
    }
    EXPECT_GT(news, 25'000U);
    EXPECT_GT(cancels, 10'000U);
    EXPECT_GT(replaces, 3'000U);
    EXPECT_GT(markets, 500U);
    EXPECT_GT(icebergs, 300U);
    EXPECT_GT(flow.last_order_id(), 25'000U);
}

TEST(OrderFlow, DrivesEngineWithFewRejections) {
    RecordingListener events;
    ManualClock clock;
    MatchingEngine engine(events, clock);
    InstrumentSpec spec;
    spec.symbol = "SIM";
    const InstrumentId id = engine.add_instrument(spec);
    sim::OrderFlowGenerator flow(config_for({id}, 21));
    for (int i = 0; i < 20'000; ++i) {
        engine.process(flow.next());
    }
    EXPECT_FALSE(engine.book(id)->check_invariants().has_value());
    EXPECT_GT(events.trades.size(), 1'000U);
    // Rejections come from cancels/replaces of orders that already traded away and from
    // fill-or-kill or post-only orders that could not be honoured; never from bad input.
    for (const OrderRejected& r : events.rejected) {
        EXPECT_TRUE(r.reason == RejectReason::UnknownOrder ||
                    r.reason == RejectReason::FillOrKillUnfillable ||
                    r.reason == RejectReason::PostOnlyWouldCross ||
                    r.reason == RejectReason::QuantityBelowExecuted ||
                    r.reason == RejectReason::NoLiquidity)
            << to_string(r.reason);
    }
}

TEST(OrderFlow, RejectsEmptyConfiguration) {
    EXPECT_THROW(sim::OrderFlowGenerator(sim::FlowConfig{}), std::invalid_argument);
}

}  // namespace
}  // namespace quill::test
