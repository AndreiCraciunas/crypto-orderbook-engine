#include <benchmark/benchmark.h>
#include "core/order_book.hpp"
#include "core/types.hpp"

using namespace marketdata;

static void BM_SnapshotCreation(benchmark::State& state) {
    OrderBook<1000> book;

    // Pre-populate order book with many levels
    for (int i = 0; i < state.range(0); ++i) {
        book.update_bid(100.0 - i * 0.1, 10.0 + i);
        book.update_ask(100.0 + i * 0.1, 10.0 + i);
    }

    for (auto _ : state) {
        auto snapshot = book.get_snapshot(state.range(0));
        benchmark::DoNotOptimize(snapshot);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetComplexityN(state.range(0));
}
BENCHMARK(BM_SnapshotCreation)->Range(10, 1000)->Complexity();

static void BM_VectorCopy(benchmark::State& state) {
    std::vector<PriceLevelSnapshot> levels;

    for (int i = 0; i < state.range(0); ++i) {
        levels.emplace_back(100.0 + i, 10.0 + i, 1);
    }

    for (auto _ : state) {
        std::vector<PriceLevelSnapshot> copy = levels;
        benchmark::DoNotOptimize(copy);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetComplexityN(state.range(0));
}
BENCHMARK(BM_VectorCopy)->Range(10, 1000)->Complexity();
