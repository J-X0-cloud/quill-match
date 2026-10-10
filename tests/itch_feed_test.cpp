// End-to-end market data checks: the book a feed consumer rebuilds from the ITCH stream
// must match the matching engine's own displayed book, level for level.

#include <gtest/gtest.h>

#include <sstream>

#include "quill/itch/book_builder.hpp"
#include "quill/itch/codec.hpp"
#include "quill/itch/publisher.hpp"
#include "quill/sim/order_flow.hpp"
#include "test_support.hpp"

namespace quill::test {
namespace {

using itch::BookBuilder;
using itch::Publisher;

std::vector<LevelSnapshot> displayed(std::vector<LevelSnapshot> levels) {
    for (auto& level : levels) {
        level.total = level.visible;  // the feed cannot see iceberg reserve
    }
    return levels;
}

class FeedFixture : public ::testing::Test {
  protected:
    FeedFixture() : engine(fanout, clock) {
        fanout.add(publisher);
        fanout.add(events);
        publisher.start_of_messages(clock.now());
        for (const char* symbol : {"AAA", "BBB", "CCC"}) {
            InstrumentSpec spec;
            spec.symbol = symbol;
            const InstrumentId id = engine.add_instrument(spec);
            publisher.register_instrument(id, spec, clock.now());
            instruments.push_back(id);
        }
    }

    BookBuilder replay() {
        BookBuilder builder;
        const auto result = itch::decode(publisher.writer().data(), builder);
        EXPECT_FALSE(result.error.has_value());
        EXPECT_EQ(result.consumed, publisher.writer().size());
        return builder;
    }

    void expect_books_match(const BookBuilder& builder) {
        for (const InstrumentId id : instruments) {
            const auto locate = static_cast<std::uint16_t>(id);
            for (Side side : {Side::Buy, Side::Sell}) {
                const auto engine_depth = displayed(engine.book(id)->depth(side, 1'000));
                const auto feed_depth = builder.depth(locate, side, 1'000);
                ASSERT_EQ(feed_depth, engine_depth)
                    << "instrument " << id << " side " << to_string(side);
            }
            EXPECT_EQ(builder.best_bid(locate), engine.book(id)->best_bid());
            EXPECT_EQ(builder.best_ask(locate), engine.book(id)->best_ask());
        }
    }

    RecordingListener events;
    Publisher publisher;
    EventFanout fanout;
    ManualClock clock{34'200'000'000'000ULL, 1'000};
    MatchingEngine engine;
    std::vector<InstrumentId> instruments;
};

TEST_F(FeedFixture, DirectoryAndSystemEventsArePublished) {
    publisher.end_of_messages(clock.now());
    const BookBuilder builder = replay();
    EXPECT_EQ(builder.stats().directory, 3U);
    EXPECT_EQ(builder.stats().system_events, 2U);
    EXPECT_TRUE(builder.end_of_messages());
    ASSERT_EQ(builder.locates().size(), 3U);
    EXPECT_EQ(builder.find("BBB"), std::uint16_t{2});
    EXPECT_EQ(builder.instrument(2)->tick_size, 100U);
    EXPECT_FALSE(builder.find("ZZZ").has_value());
}

TEST_F(FeedFixture, IcebergRefillsAppearAsFreshAdds) {
    NewOrderRequest berg;
    berg.instrument = instruments[0];
    berg.side = Side::Sell;
    berg.price = px(10.00);
    berg.quantity = 300;
    berg.display_quantity = 100;
    engine.submit(berg);

    NewOrderRequest hit;
    hit.instrument = instruments[0];
    hit.side = Side::Buy;
    hit.price = px(10.00);
    hit.quantity = 150;
    engine.submit(hit);

    const BookBuilder builder = replay();
    EXPECT_EQ(builder.stats().anomalies, 0U);
    const auto asks = builder.depth(1, Side::Sell, 5);
    ASSERT_EQ(asks.size(), 1U);
    EXPECT_EQ(asks[0].visible, 50);  // second tranche of 100, 50 already taken
    EXPECT_EQ(builder.instrument(1)->executed_volume, 150);
    EXPECT_EQ(builder.instrument(1)->last_price, px(10.00));
    expect_books_match(builder);
}

TEST_F(FeedFixture, RandomFlowReplaysToIdenticalBooks) {
    sim::FlowConfig config;
    config.instruments = instruments;
    config.seed = 77;
    config.price_band_ticks = 6;
    sim::OrderFlowGenerator flow(config);
    for (int i = 0; i < 40'000; ++i) {
        engine.process(flow.next());
        if (i % 5'000 == 0) {
            expect_books_match(replay());
        }
    }
    const BookBuilder builder = replay();
    EXPECT_EQ(builder.stats().anomalies, 0U);
    EXPECT_GT(builder.stats().executions, 1'000U);
    EXPECT_GT(builder.stats().cancels, 0U);  // in-place size reductions
    expect_books_match(builder);

    std::size_t engine_orders = 0;
    Quantity traded = 0;
    for (const InstrumentId id : instruments) {
        engine_orders += engine.book(id)->order_count();
        traded += builder.instrument(static_cast<std::uint16_t>(id))->executed_volume;
    }
    EXPECT_EQ(builder.live_orders(), engine_orders);
    EXPECT_EQ(traded, events.traded_volume());
}

TEST_F(FeedFixture, FlushDrainsToStream) {
    std::ostringstream out;
    const std::size_t pending = publisher.writer().size();
    publisher.flush(out);
    EXPECT_EQ(out.str().size(), pending);
    EXPECT_EQ(publisher.writer().size(), 0U);
}

TEST(BookBuilder, CountsAnomaliesInsteadOfTrusting) {
    BookBuilder builder;
    const itch::Header h{1, 0, 0};
    builder.on_message(itch::OrderDelete{h, 5});
    builder.on_message(itch::OrderExecuted{h, 5, 10, 1});
    builder.on_message(itch::AddOrder{h, 5, Side::Buy, 10, itch::make_symbol("X"), 100});
    builder.on_message(itch::AddOrder{h, 5, Side::Buy, 10, itch::make_symbol("X"), 100});
    builder.on_message(itch::OrderCancel{h, 5, 11});
    builder.on_message(itch::OrderReplace{h, 6, 7, 1, 1});
    EXPECT_EQ(builder.stats().anomalies, 5U);
    EXPECT_EQ(builder.live_orders(), 1U);
    EXPECT_EQ(builder.depth(1, Side::Buy, 1)[0].visible, 10);
}

TEST(BookBuilder, ReplaceMovesOrderAndKeepsSide) {
    BookBuilder builder;
    const itch::Header h{3, 0, 0};
    builder.on_message(itch::AddOrder{h, 1, Side::Sell, 10, itch::make_symbol("X"), 500});
    builder.on_message(itch::AddOrder{h, 2, Side::Sell, 5, itch::make_symbol("X"), 500});
    builder.on_message(itch::OrderReplace{h, 1, 9, 20, 400});
    const auto asks = builder.depth(3, Side::Sell, 5);
    ASSERT_EQ(asks.size(), 2U);
    EXPECT_EQ(asks[0], (LevelSnapshot{400, 20, 20, 1}));
    EXPECT_EQ(asks[1], (LevelSnapshot{500, 5, 5, 1}));
    builder.on_message(itch::OrderExecuted{h, 9, 20, 1});
    EXPECT_EQ(builder.best_ask(3), Price{500});
    EXPECT_EQ(builder.live_orders(), 1U);
}

TEST(BookBuilder, UnknownLocateHasNoDepth) {
    BookBuilder builder;
    EXPECT_TRUE(builder.depth(9, Side::Buy, 5).empty());
    EXPECT_FALSE(builder.best_bid(9).has_value());
    EXPECT_EQ(builder.instrument(9), nullptr);
}

}  // namespace
}  // namespace quill::test
