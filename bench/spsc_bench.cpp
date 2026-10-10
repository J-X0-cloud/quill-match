// Cross-thread queue throughput. The mutex-protected deque is included as a baseline so
// the SPSC numbers have something honest to be compared against.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "quill/commands.hpp"
#include "quill/spsc_queue.hpp"

namespace {

constexpr std::uint64_t kItemsPerRun = 1'000'000;

template <typename Push, typename Pop>
void run_pair(benchmark::State& state, Push push, Pop pop) {
    for (auto _ : state) {
        std::thread consumer([&] {
            std::uint64_t received = 0;
            while (received < kItemsPerRun) {
                if (pop()) {
                    ++received;
                } else {
                    quill::cpu_relax();
                }
            }
        });
        for (std::uint64_t i = 0; i < kItemsPerRun; ++i) {
            while (!push(i)) {
                quill::cpu_relax();
            }
        }
        consumer.join();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kItemsPerRun));
}

void BM_SpscQueueU64(benchmark::State& state) {
    quill::SpscQueue<std::uint64_t> queue(static_cast<std::size_t>(state.range(0)));
    run_pair(
        state, [&](std::uint64_t v) { return queue.try_push(v); },
        [&] {
            std::uint64_t v = 0;
            return queue.try_pop(v);
        });
}
BENCHMARK(BM_SpscQueueU64)->Arg(1'024)->Arg(65'536)->UseRealTime()->Unit(benchmark::kMillisecond);

void BM_SpscQueueCommand(benchmark::State& state) {
    quill::SpscQueue<quill::Command> queue(65'536);
    quill::Command command{quill::NewOrderRequest{}, 0};
    run_pair(
        state, [&](std::uint64_t) { return queue.try_push(command); },
        [&] {
            quill::Command* front = queue.front();
            if (front == nullptr) {
                return false;
            }
            benchmark::DoNotOptimize(front->payload.index());
            queue.pop();
            return true;
        });
    state.counters["bytes_per_item"] = static_cast<double>(sizeof(quill::Command));
}
BENCHMARK(BM_SpscQueueCommand)->UseRealTime()->Unit(benchmark::kMillisecond);

void BM_MutexDequeU64(benchmark::State& state) {
    std::mutex mutex;
    std::deque<std::uint64_t> queue;
    run_pair(
        state,
        [&](std::uint64_t v) {
            const std::lock_guard lock(mutex);
            if (queue.size() >= 65'536) {
                return false;
            }
            queue.push_back(v);
            return true;
        },
        [&] {
            const std::lock_guard lock(mutex);
            if (queue.empty()) {
                return false;
            }
            queue.pop_front();
            return true;
        });
}
BENCHMARK(BM_MutexDequeU64)->UseRealTime()->Unit(benchmark::kMillisecond);

}  // namespace
