#include <benchmark/benchmark.h>
#include "core/order_book.hpp"
#include "utils/time_utils.hpp"
#include <random>
#include <thread>
#include <vector>
#include <atomic>

using namespace marketdata;

static void BM_OrderBookUpdate(benchmark::State& state) {
    OrderBook<1000> book;
    std::mt19937 gen(42);
    std::uniform_real_distribution<> price_dist(90.0, 110.0);
    std::uniform_real_distribution<> qty_dist(0.1, 100.0);

    for (auto _ : state) {
        double price = price_dist(gen);
        double qty = qty_dist(gen);

        if (price < 100.0) {
            book.update_bid(price, qty);
        } else {
            book.update_ask(price, qty);
        }
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("updates/sec");
}
BENCHMARK(BM_OrderBookUpdate);

static void BM_OrderBookSnapshot(benchmark::State& state) {
    OrderBook<1000> book;

    // Pre-populate order book
    for (double price = 90.0; price < 100.0; price += 0.1) {
        book.update_bid(price, price * 10);
    }
    for (double price = 100.0; price < 110.0; price += 0.1) {
        book.update_ask(price, price * 10);
    }

    for (auto _ : state) {
        auto snapshot = book.get_snapshot(20);
        benchmark::DoNotOptimize(snapshot);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("snapshots/sec");
}
BENCHMARK(BM_OrderBookSnapshot);

static void BM_OrderBookUpdateWithSnapshot(benchmark::State& state) {
    OrderBook<1000> book;
    std::mt19937 gen(42);
    std::uniform_real_distribution<> price_dist(90.0, 110.0);
    std::uniform_real_distribution<> qty_dist(0.1, 100.0);

    int update_count = 0;

    for (auto _ : state) {
        double price = price_dist(gen);
        double qty = qty_dist(gen);

        if (price < 100.0) {
            book.update_bid(price, qty);
        } else {
            book.update_ask(price, qty);
        }

        update_count++;

        // Take snapshot every 100 updates
        if (update_count % 100 == 0) {
            auto snapshot = book.get_snapshot(20);
            benchmark::DoNotOptimize(snapshot);
        }
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookUpdateWithSnapshot);

static void BM_GetSpread(benchmark::State& state) {
    OrderBook<1000> book;
    book.update_bid(100.0, 10.0);
    book.update_ask(100.5, 15.0);

    for (auto _ : state) {
        double spread = book.get_spread();
        benchmark::DoNotOptimize(spread);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GetSpread);

static void BM_GetMidPrice(benchmark::State& state) {
    OrderBook<1000> book;
    book.update_bid(100.0, 10.0);
    book.update_ask(100.5, 15.0);

    for (auto _ : state) {
        double mid = book.get_mid_price();
        benchmark::DoNotOptimize(mid);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GetMidPrice);

static void BM_GetStatistics(benchmark::State& state) {
    OrderBook<1000> book;

    // Pre-populate
    for (double price = 90.0; price < 100.0; price += 0.5) {
        book.update_bid(price, price * 10);
    }
    for (double price = 100.0; price < 110.0; price += 0.5) {
        book.update_ask(price, price * 10);
    }

    for (auto _ : state) {
        auto stats = book.get_statistics();
        benchmark::DoNotOptimize(stats);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GetStatistics);

static void BM_ConcurrentOrderBookUpdates(benchmark::State& state) {
    OrderBook<1000> book;

    for (auto _ : state) {
        state.PauseTiming();

        std::vector<std::thread> threads;
        std::atomic<bool> start{false};

        for (int i = 0; i < state.range(0); ++i) {
            threads.emplace_back([&book, &start, i]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                std::mt19937 gen(i);
                std::uniform_real_distribution<> price_dist(90.0, 110.0);
                std::uniform_real_distribution<> qty_dist(0.1, 100.0);

                for (int j = 0; j < 1000; ++j) {
                    double price = price_dist(gen);
                    double qty = qty_dist(gen);

                    if (price < 100.0) {
                        book.update_bid(price, qty);
                    } else {
                        book.update_ask(price, qty);
                    }
                }
            });
        }

        state.ResumeTiming();
        start.store(true, std::memory_order_release);

        for (auto& t : threads) {
            t.join();
        }
    }

    state.SetItemsProcessed(state.iterations() * state.range(0) * 1000);
}
BENCHMARK(BM_ConcurrentOrderBookUpdates)->Range(1, 16);

static void BM_PriceIndexMapInsert(benchmark::State& state) {
    PriceIndexMap map;
    uint64_t price = 0;

    for (auto _ : state) {
        map.insert(price++, price % 1000);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PriceIndexMapInsert);

static void BM_PriceIndexMapFind(benchmark::State& state) {
    PriceIndexMap map;

    // Pre-populate
    for (uint64_t i = 0; i < 1000; ++i) {
        map.insert(i * 100, i);
    }

    std::mt19937 gen(42);
    std::uniform_int_distribution<uint64_t> dist(0, 999);

    for (auto _ : state) {
        uint64_t price = dist(gen) * 100;
        auto result = map.find(price);
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PriceIndexMapFind);

static void BM_FixedPointConversion(benchmark::State& state) {
    double value = 123.456789;

    for (auto _ : state) {
        uint64_t fixed = double_to_fixed(value);
        double converted = fixed_to_double(fixed);
        benchmark::DoNotOptimize(converted);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixedPointConversion);
