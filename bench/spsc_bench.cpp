#include <benchmark/benchmark.h>
#include "core/spsc_queue.h"

using namespace minitrader;

// 单线程 push/pop 往返吞吐测试
static void BM_SPSCPushPop(benchmark::State& state) {
    SPSCQueue<int64_t, 1024> q;

    for (auto _ : state) {
        std::ignore = q.try_push(42);
        auto val = q.try_pop();
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_SPSCPushPop);

// 突发写后突发读（模拟行情批量到达）
static void BM_SPSCBurst(benchmark::State& state) {
    SPSCQueue<int64_t, 1024> q;
    const int burst_size = static_cast<int>(state.range(0));

    for (auto _ : state) {
        for (int i = 0; i < burst_size; ++i) {
            std::ignore = q.try_push(i);
        }
        for (int i = 0; i < burst_size; ++i) {
            auto val = q.try_pop();
            benchmark::DoNotOptimize(val);
        }
    }
    state.SetItemsProcessed(state.iterations() * burst_size);
}
BENCHMARK(BM_SPSCBurst)->Range(8, 512);

// 传输真实 Order 尺寸结构体（32 字节）的 push/pop 开销
struct FakeOrder {
    uint64_t id;
    int64_t  price;
    int32_t  qty;
    uint64_t ts;
};

static void BM_SPSCOrder(benchmark::State& state) {
    SPSCQueue<FakeOrder, 1024> q;

    for (auto _ : state) {
        std::ignore = q.try_push(FakeOrder{1, 15000, 100, 123456789});
        auto val = q.try_pop();
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_SPSCOrder);

BENCHMARK_MAIN();
