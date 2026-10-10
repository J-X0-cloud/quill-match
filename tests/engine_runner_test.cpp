#include "quill/engine_runner.hpp"

#include <gtest/gtest.h>

#include <random>
#include <stdexcept>
#include <vector>

#include "test_support.hpp"

namespace quill::test {
namespace {

std::vector<Command> random_flow(InstrumentId instrument, std::size_t count, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<Command> commands;
    OrderId next_id = 1;
    for (std::size_t i = 0; i < count; ++i) {
        if (next_id > 10 && rng() % 4 == 0) {
            const OrderId target = 1 + rng() % (next_id - 1);
            commands.push_back(Command{CancelRequest{instrument, target, 0}, 0});
            continue;
        }
        NewOrderRequest request;
        request.instrument = instrument;
        request.order_id = next_id++;
        request.side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        request.price = px(100.00) + static_cast<Price>(rng() % 11) * 100 - 500;
        request.quantity = 1 + static_cast<Quantity>(rng() % 50);
        commands.push_back(Command{request, 0});
    }
    return commands;
}

TEST(EngineRunner, ThreadedRunMatchesSynchronousRun) {
    InstrumentSpec spec;
    spec.symbol = "RUN";

    RecordingListener sync_events;
    ManualClock sync_clock;
    MatchingEngine sync_engine(sync_events, sync_clock);
    const InstrumentId sync_id = sync_engine.add_instrument(spec);

    RecordingListener threaded_events;
    ManualClock threaded_clock;
    MatchingEngine threaded_engine(threaded_events, threaded_clock);
    const InstrumentId threaded_id = threaded_engine.add_instrument(spec);
    ASSERT_EQ(sync_id, threaded_id);

    const auto commands = random_flow(sync_id, 50'000, 3);
    for (const Command& c : commands) {
        sync_engine.process(c);
    }

    EngineRunner runner(threaded_engine, 256);
    runner.start();
    for (const Command& c : commands) {
        runner.submit(c);
    }
    runner.stop();

    EXPECT_EQ(runner.processed(), commands.size());
    EXPECT_EQ(runner.latency().count(), commands.size());
    EXPECT_EQ(threaded_events.sequence, sync_events.sequence);
    ASSERT_EQ(threaded_events.trades.size(), sync_events.trades.size());
    for (std::size_t i = 0; i < sync_events.trades.size(); ++i) {
        EXPECT_EQ(threaded_events.trades[i].resting_id, sync_events.trades[i].resting_id);
        EXPECT_EQ(threaded_events.trades[i].quantity, sync_events.trades[i].quantity);
    }
    EXPECT_EQ(threaded_engine.book(threaded_id)->depth(Side::Buy, 20),
              sync_engine.book(sync_id)->depth(Side::Buy, 20));
}

TEST(EngineRunner, StopDrainsQueuedCommands) {
    RecordingListener events;
    ManualClock clock;
    MatchingEngine engine(events, clock);
    InstrumentSpec spec;
    spec.symbol = "DRAIN";
    const InstrumentId id = engine.add_instrument(spec);

    EngineRunner runner(engine, 1024);
    // Fill the queue before the consumer exists, then start and immediately stop.
    for (int i = 0; i < 1000; ++i) {
        NewOrderRequest request;
        request.instrument = id;
        request.price = px(10.00);
        request.quantity = 1;
        ASSERT_TRUE(runner.try_submit(Command{request, 0}));
    }
    runner.start();
    runner.stop();
    EXPECT_EQ(runner.processed(), 1000U);
    EXPECT_EQ(engine.book(id)->order_count(), 1000U);
    EXPECT_FALSE(runner.running());
}

TEST(EngineRunner, TrySubmitReportsBackPressure) {
    RecordingListener events;
    ManualClock clock;
    MatchingEngine engine(events, clock);
    EngineRunner runner(engine, 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(runner.try_submit(Command{CancelRequest{1, 1, 0}, 0}));
    }
    EXPECT_FALSE(runner.try_submit(Command{CancelRequest{1, 1, 0}, 0}));
}

TEST(EngineRunner, DoubleStartIsAnError) {
    RecordingListener events;
    ManualClock clock;
    MatchingEngine engine(events, clock);
    EngineRunner runner(engine, 4);
    runner.start();
    EXPECT_THROW(runner.start(), std::logic_error);
    runner.stop();
    runner.stop();  // idempotent
}

}  // namespace
}  // namespace quill::test
