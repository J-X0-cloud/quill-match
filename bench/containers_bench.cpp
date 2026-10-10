#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "quill/flat_hash_map.hpp"
#include "quill/latency_histogram.hpp"

namespace {

using quill::FlatHashMap;

std::vector<std::uint64_t> lookup_keys(std::size_t live, std::size_t count) {
    std::mt19937_64 rng(3);
    std::vector<std::uint64_t> keys(count);
    for (auto& k : keys) {
        k = 1 + rng() % live;
    }
    return keys;
}

template <typename Map>
void lookups(benchmark::State& state, Map& map) {
    const auto live = static_cast<std::size_t>(state.range(0));
    for (std::uint64_t k = 1; k <= live; ++k) {
        map[k] = k;
    }
    const auto keys = lookup_keys(live, 1 << 16);
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(map.find(keys[i++ & 0xFFFF]));
    }
    state.SetItemsProcessed(state.iterations());
}

// Small adaptor so FlatHashMap can share the fill code above.
struct FlatAdaptor {
    FlatHashMap<std::uint64_t, std::uint64_t> map;
    std::uint64_t& operator[](std::uint64_t k) {
        map.insert(k, 0);
        return *map.find(k);
    }
    auto find(std::uint64_t k) { return map.find(k); }
};

void BM_FlatHashMapFind(benchmark::State& state) {
    FlatAdaptor map;
    lookups(state, map);
}
BENCHMARK(BM_FlatHashMapFind)->Arg(1'000)->Arg(1'000'000);

void BM_StdUnorderedMapFind(benchmark::State& state) {
    std::unordered_map<std::uint64_t, std::uint64_t> map;
    lookups(state, map);
}
BENCHMARK(BM_StdUnorderedMapFind)->Arg(1'000)->Arg(1'000'000);

// Insert/erase churn with a sliding window of live keys, the order-id index workload.
void BM_FlatHashMapChurn(benchmark::State& state) {
    FlatHashMap<std::uint64_t, std::uint64_t> map;
    std::uint64_t next = 1;
    for (; next <= 100'000; ++next) {
        map.insert(next, next);
    }
    for (auto _ : state) {
        map.insert(next, next);
        map.erase(next - 100'000);
        ++next;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_FlatHashMapChurn);

void BM_StdUnorderedMapChurn(benchmark::State& state) {
    std::unordered_map<std::uint64_t, std::uint64_t> map;
    std::uint64_t next = 1;
    for (; next <= 100'000; ++next) {
        map.emplace(next, next);
    }
    for (auto _ : state) {
        map.emplace(next, next);
        map.erase(next - 100'000);
        ++next;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_StdUnorderedMapChurn);

void BM_HistogramRecord(benchmark::State& state) {
    quill::LatencyHistogram histogram;
    std::mt19937_64 rng(1);
    std::vector<std::uint64_t> values(4096);
    for (auto& v : values) {
        v = 100 + rng() % 100'000;
    }
    std::size_t i = 0;
    for (auto _ : state) {
        histogram.record(values[i++ & 4095]);
    }
    std::uint64_t count = histogram.count();
    benchmark::DoNotOptimize(count);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HistogramRecord);

}  // namespace
