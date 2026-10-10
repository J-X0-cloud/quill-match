#include <benchmark/benchmark.h>

#include <string>

#include "quill/fix/message.hpp"

namespace {

using namespace quill::fix;

std::string new_order_single() {
    Builder builder(msg::kNewOrderSingle);
    builder.add(tag::kSenderCompId, std::string_view("CLIENT01"))
        .add(tag::kTargetCompId, std::string_view("QUILL"))
        .add(tag::kMsgSeqNum, std::int64_t{1234})
        .add(tag::kSendingTime, std::string_view("20260101-12:00:00.000"))
        .add(tag::kClOrdId, std::string_view("ORD-0000001234"))
        .add(tag::kSymbol, std::string_view("AAPL"))
        .add(tag::kSide, '1')
        .add(tag::kOrderQty, std::int64_t{500})
        .add(tag::kOrdType, '2')
        .add(tag::kPrice, std::string_view("189.25"))
        .add(tag::kTimeInForce, '0');
    return builder.finish();
}

void BM_FixParseNewOrderStrict(benchmark::State& state) {
    const std::string raw = new_order_single();
    for (auto _ : state) {
        auto result = Message::parse(raw);
        benchmark::DoNotOptimize(result.message);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(raw.size()));
}
BENCHMARK(BM_FixParseNewOrderStrict);

void BM_FixBuildExecutionReport(benchmark::State& state) {
    for (auto _ : state) {
        Builder builder(msg::kExecutionReport);
        builder.add(tag::kSenderCompId, std::string_view("QUILL"))
            .add(tag::kTargetCompId, std::string_view("CLIENT01"))
            .add(tag::kMsgSeqNum, std::int64_t{99})
            .add(tag::kOrderId, std::int64_t{123456})
            .add(tag::kClOrdId, std::string_view("ORD-0000001234"))
            .add(tag::kExecType, 'F')
            .add(tag::kOrdStatus, '1')
            .add(tag::kLastPx, format_price(1'892'500))
            .add(tag::kLastQty, std::int64_t{100})
            .add(tag::kLeavesQty, std::int64_t{400})
            .add(tag::kCumQty, std::int64_t{100});
        std::string text = builder.finish();
        benchmark::DoNotOptimize(text);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixBuildExecutionReport);

void BM_FixParsePrice(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(parse_price("189.2575"));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixParsePrice);

}  // namespace
