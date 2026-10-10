#include <benchmark/benchmark.h>

#include <vector>

#include "quill/clock.hpp"
#include "quill/itch/book_builder.hpp"
#include "quill/itch/codec.hpp"
#include "quill/itch/publisher.hpp"
#include "quill/matching_engine.hpp"
#include "quill/sim/order_flow.hpp"

namespace {

using namespace quill;

/// A realistic feed: the market data produced by 200k simulated commands.
const itch::Writer& sample_feed() {
    static const itch::Writer feed = [] {
        itch::Publisher publisher;
        ManualClock clock;
        MatchingEngine engine(publisher, clock);
        std::vector<InstrumentId> ids;
        for (int i = 0; i < 4; ++i) {
            InstrumentSpec spec;
            spec.symbol = "S" + std::to_string(i);
            ids.push_back(engine.add_instrument(spec));
            publisher.register_instrument(ids.back(), spec, 0);
        }
        sim::FlowConfig config;
        config.instruments = ids;
        sim::OrderFlowGenerator generator(config);
        for (int i = 0; i < 200'000; ++i) {
            engine.process(generator.next());
        }
        return publisher.writer();
    }();
    return feed;
}

struct CountingHandler {
    std::uint64_t shares = 0;
    void on_message(const itch::AddOrder& m) { shares += m.shares; }
    void on_message(const itch::OrderExecuted& m) { shares += m.executed_shares; }
    template <typename M>
    void on_message(const M& /*message*/) {}
};

void BM_ItchEncodeAddOrder(benchmark::State& state) {
    itch::Writer writer;
    const itch::AddOrder message{itch::Header{1, 0, 123},   42,       Side::Buy, 100,
                                 itch::make_symbol("AAPL"), 1'895'000};
    for (auto _ : state) {
        writer.write(message);
        if (writer.size() > (1U << 20)) {
            writer.clear();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchEncodeAddOrder);

void BM_ItchDecodeOnly(benchmark::State& state) {
    const auto& feed = sample_feed();
    for (auto _ : state) {
        CountingHandler handler;
        auto result = itch::decode(feed.data(), handler);
        benchmark::DoNotOptimize(handler.shares);
        benchmark::DoNotOptimize(result.messages);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(feed.messages()));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(feed.size()));
}
BENCHMARK(BM_ItchDecodeOnly)->Unit(benchmark::kMillisecond);

void BM_ItchReplayIntoBooks(benchmark::State& state) {
    const auto& feed = sample_feed();
    for (auto _ : state) {
        itch::BookBuilder builder;
        auto result = itch::decode(feed.data(), builder);
        benchmark::DoNotOptimize(result.messages);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(feed.messages()));
}
BENCHMARK(BM_ItchReplayIntoBooks)->Unit(benchmark::kMillisecond);

}  // namespace
