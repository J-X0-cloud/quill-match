// Order book micro-benchmarks. Each benchmark builds its book once outside the timed loop
// and then performs a self-restoring operation per iteration, so the book stays the same
// size for the whole run and PauseTiming is never needed.

#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "quill/clock.hpp"
#include "quill/matching_engine.hpp"
#include "quill/sim/order_flow.hpp"

namespace {

using namespace quill;

/// Swallows events without doing work, so the benchmark measures the book itself.
class NullListener final : public EventListener {};

class BenchEngine {
  public:
    BenchEngine() : engine(listener, clock) {
        InstrumentSpec spec;
        spec.symbol = "BENCH";
        spec.max_order_quantity = 1'000'000'000;
        id = engine.add_instrument(spec);
    }

    OrderId limit(Side side, Price price, Quantity quantity,
                  TimeInForce tif = TimeInForce::GoodTillCancel) {
        NewOrderRequest r;
        r.instrument = id;
        r.side = side;
        r.price = price;
        r.quantity = quantity;
        r.tif = tif;
        return engine.submit(r);
    }

    /// Rests `levels` bid and ask levels with `per_level` orders each around 100.00.
    void populate(int levels, int per_level) {
        for (int l = 1; l <= levels; ++l) {
            for (int k = 0; k < per_level; ++k) {
                limit(Side::Buy, 1'000'000 - l * 100, 100);
                limit(Side::Sell, 1'000'000 + l * 100, 100);
            }
        }
    }

    NullListener listener;
    ManualClock clock;
    MatchingEngine engine;
    InstrumentId id = 0;
};

// Insert a passive order behind the touch and cancel it again.
void BM_AddCancelPassive(benchmark::State& state) {
    BenchEngine b;
    b.populate(static_cast<int>(state.range(0)), 10);
    for (auto _ : state) {
        const OrderId id = b.limit(Side::Buy, 1'000'000 - 300, 100);
        b.engine.cancel(CancelRequest{b.id, id, 0});
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddCancelPassive)->Arg(10)->Arg(1'000);

// Insert at a random depth (forces binary search and mid-vector inserts of new levels).
void BM_AddCancelRandomLevel(benchmark::State& state) {
    BenchEngine b;
    b.populate(500, 4);
    std::mt19937 rng(1);
    std::vector<Price> prices(4096);
    for (Price& p : prices) {
        // Half the draws land on existing levels, half on empty ticks between them.
        p = 1'000'000 - 50 - static_cast<Price>(rng() % 1'000) * 50;
    }
    std::size_t i = 0;
    for (auto _ : state) {
        const OrderId id = b.limit(Side::Buy, prices[i++ & 4095], 100);
        b.engine.cancel(CancelRequest{b.id, id, 0});
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddCancelRandomLevel);

// An aggressive order that consumes `fills` resting orders, which are then replenished.
void BM_SweepFills(benchmark::State& state) {
    const auto fills = static_cast<int>(state.range(0));
    BenchEngine b;
    b.populate(50, 5);
    for (auto _ : state) {
        for (int k = 0; k < fills; ++k) {
            b.limit(Side::Sell, 999'900, 10);
        }
        b.limit(Side::Buy, 999'900, 10 * fills);
    }
    state.SetItemsProcessed(state.iterations() * (fills + 1));
    state.counters["fills_per_iter"] = fills;
}
BENCHMARK(BM_SweepFills)->Arg(1)->Arg(10)->Arg(50);

// Cancel/replace: size-down in place versus re-price (loses priority, re-queues).
void BM_ReplaceKeepPriority(benchmark::State& state) {
    BenchEngine b;
    b.populate(100, 10);
    const OrderId id = b.limit(Side::Buy, 990'000, 1'000'000'000);
    Quantity quantity = 1'000'000'000;
    for (auto _ : state) {
        b.engine.replace(ReplaceRequest{b.id, id, 0, 990'000, --quantity});
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReplaceKeepPriority);

void BM_ReplaceReprice(benchmark::State& state) {
    BenchEngine b;
    b.populate(100, 10);
    const OrderId id = b.limit(Side::Buy, 990'000, 100);
    bool flip = false;
    for (auto _ : state) {
        b.engine.replace(ReplaceRequest{b.id, id, 0, flip ? 990'000 : 989'900, 100});
        flip = !flip;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReplaceReprice);

// Iceberg refill: each iteration takes one full peak, which re-queues the iceberg.
void BM_IcebergRefill(benchmark::State& state) {
    BenchEngine b;
    b.populate(20, 5);
    NewOrderRequest berg;
    berg.instrument = b.id;
    berg.side = Side::Sell;
    berg.price = 999'000;
    berg.quantity = 900'000'000;
    berg.display_quantity = 100;
    b.engine.submit(berg);
    b.limit(Side::Sell, 999'000, 100);
    for (auto _ : state) {
        b.limit(Side::Buy, 999'000, 100, TimeInForce::ImmediateOrCancel);
        b.limit(Side::Sell, 999'000, 100);  // keep the plain order count constant
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_IcebergRefill);

// Realistic mixed flow from the simulator, replayed through a fresh engine per batch.
void BM_MixedFlow(benchmark::State& state) {
    sim::FlowConfig config;
    config.instruments = {1, 2, 3, 4};
    config.seed = 7;
    sim::OrderFlowGenerator generator(config);
    const std::vector<Command> commands = generator.generate(200'000);
    for (auto _ : state) {
        NullListener listener;
        ManualClock clock;
        MatchingEngine engine(listener, clock);
        for (int i = 0; i < 4; ++i) {
            InstrumentSpec spec;
            spec.symbol = "S" + std::to_string(i);
            engine.add_instrument(spec);
        }
        for (const Command& c : commands) {
            engine.process(c);
        }
        MatchId last = engine.last_match_id();
        benchmark::DoNotOptimize(last);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(commands.size()));
}
BENCHMARK(BM_MixedFlow)->Unit(benchmark::kMillisecond);

}  // namespace
