#include <benchmark/benchmark.h>
#include "core/spsc_queue.h"

using namespace minitrader;

// Benchmark: single-threaded push/pop throughput
static void BM_SPSCPushPop(benchmark::State& state) {
    SPSCQueue<int64_t, 1024> q;

    for (auto _ : state) {
        q.try_push(42);
        auto val = q.try_pop();
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_SPSCPushPop);

// Benchmark: burst write then burst read
static void BM_SPSCBurst(benchmark::State& state) {
    SPSCQueue<int64_t, 1024> q;
    const int burst_size = static_cast<int>(state.range(0));

    for (auto _ : state) {
        for (int i = 0; i < burst_size; ++i) {
            q.try_push(i);
        }
        for (int i = 0; i < burst_size; ++i) {
            auto val = q.try_pop();
            benchmark::DoNotOptimize(val);
        }
    }
    state.SetItemsProcessed(state.iterations() * burst_size);
}
BENCHMARK(BM_SPSCBurst)->Range(8, 512);

// Benchmark: struct (simulating Order) push/pop
struct FakeOrder {
    uint64_t id;
    int64_t price;
    int32_t qty;
    uint64_t ts;
};

static void BM_SPSCOrder(benchmark::State& state) {
    SPSCQueue<FakeOrder, 1024> q;

    for (auto _ : state) {
        q.try_push(FakeOrder{1, 15000, 100, 123456789});
        auto val = q.try_pop();
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_SPSCOrder);

BENCHMARK_MAIN();
