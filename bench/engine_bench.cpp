#include <benchmark/benchmark.h>

#include <algorithm>
#include <tuple>
#include <vector>

#include "engine/trading_engine.h"
#include "strategy/spread_strategy.h"

using namespace minitrader;

// ── helpers ──────────────────────────────────────────────────────────────────

static EngineConfig make_config() {
    EngineConfig c;
    c.book_config        = {.min_price = 1, .max_price = 100000, .tick_size = 1};
    c.risk_config        = {.max_position = 10000,
                            .max_orders_per_second = 100000,
                            .max_cancel_ratio = 0.99,
                            .check_self_trade = false};
    c.enable_latency_log = false;
    return c;
}

static SpreadStrategyConfig make_strategy_cfg() {
    return SpreadStrategyConfig{
        .instrument_id = 1,
        .half_spread   = 2,
        .order_size    = 5,
        .verbose       = false,  // silence output in benchmarks
    };
}

static MarketTick make_tick(int64_t bid, int64_t ask) {
    return MarketTick{1, bid, ask, 100, 100,
                      (bid + ask) / 2, 10,
                      Order::now_ns(), Order::now_ns()};
}

// ── BM_EngineTickNoOrder ──────────────────────────────────────────────────────
// Steady-state: tick arrives, both sides already quoted → on_tick returns
// immediately without submitting orders.
static void BM_EngineTickNoOrder(benchmark::State& state) {
    TradingEngine  engine(make_config());
    SpreadStrategy strategy(make_strategy_cfg());
    engine.set_strategy(&strategy);

    // Pre-quote so on_tick sees live orders and skips submission
    std::ignore = engine.push_tick(make_tick(9998, 10002));
    std::ignore = engine.run_once();

    for (auto _ : state) {
        std::ignore = engine.push_tick(make_tick(9998, 10002));
        std::ignore = engine.run_once();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EngineTickNoOrder);

// ── BM_EngineTickWithOrder ────────────────────────────────────────────────────
// Hot path: tick arrives on flat book → strategy submits 2 limit orders
// (2× risk check + 2× book insert + 2× map registration).
//
// We cancel the resting quotes under PauseTiming so the next timed tick
// always finds an empty book and triggers order submission.
static void BM_EngineTickWithOrder(benchmark::State& state) {
    TradingEngine  engine(make_config());
    SpreadStrategy strategy(make_strategy_cfg());
    engine.set_strategy(&strategy);

    // Prime: first tick quotes both sides
    std::ignore = engine.push_tick(make_tick(9998, 10002));
    std::ignore = engine.run_once();

    for (auto _ : state) {
        // Timed: flat book → on_tick submits bid + ask
        std::ignore = engine.push_tick(make_tick(9998, 10002));
        std::ignore = engine.run_once();

        // Untimed: cancel both resting orders so next iteration starts flat
        state.PauseTiming();
        strategy.cancel_resting_quotes(engine);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EngineTickWithOrder);

// ── BM_EngineTickRequote ──────────────────────────────────────────────────────
// 路径B：每 tick 都撤单重新报价（cancel×2 + submit×2）。
// 策略的 early-return 条件是 bid_order_id_ != 0，因此需要在 PauseTiming 内
// 清掉 order_id，让下一个 tick 必然走完整报价路径。
// 与 BM_EngineTickNoOrder 差值 = cancel×2 + submit×2（2× 风控 + 2× 簿操作）的增量开销。
static void BM_EngineTickRequote(benchmark::State& state) {
    TradingEngine  engine(make_config());
    SpreadStrategy strategy(make_strategy_cfg());
    engine.set_strategy(&strategy);

    int64_t px = 9998;
    for (auto _ : state) {
        // 计时：空簿 → submit×2
        std::ignore = engine.push_tick(make_tick(px, px + 4));
        std::ignore = engine.run_once();

        // 非计时：撤掉刚报的双边挂单，下次迭代重新触发报价
        state.PauseTiming();
        strategy.cancel_resting_quotes(engine);
        ++px;
        if (px > 10000) px = 9990;  // 价格环绕，避免越界
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EngineTickRequote);

// ── BM_StrategyOnTickOnly ─────────────────────────────────────────────────────
// 纯策略逻辑开销：绕开 SPSC + OrderBook + RiskGate，直接调用 on_tick()。
// 用来隔离"策略本身（EMA + skew 计算 + 条件判断）"占总延迟的比例。
static void BM_StrategyOnTickOnly(benchmark::State& state) {
    // 用一个"永远不会下单"的空引擎支撑策略调用，但不经过 SPSC
    TradingEngine  engine(make_config());
    SpreadStrategy strategy(make_strategy_cfg());
    engine.set_strategy(&strategy);

    // 预热：先触发一次，让 ema_initialized_ = true，进入稳态
    MarketTick tick = make_tick(9998, 10002);
    strategy.on_tick(tick);
    // 清掉下出去的单，让后续 on_tick 走"已有挂单→return"快路径
    strategy.cancel_resting_quotes(engine);
    std::ignore = engine.push_tick(tick); std::ignore = engine.run_once();

    for (auto _ : state) {
        strategy.on_tick(tick);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StrategyOnTickOnly);

// ── BM_EngineLatencyHistogram ─────────────────────────────────────────────────
// Collects N per-tick wall-clock samples and reports percentile distribution
// via benchmark counters.  Measures the full push_tick + run_once round-trip.
//
// Steady-state only (both sides quoted after the first tick).
static void BM_EngineLatencyHistogram(benchmark::State& state) {
    const int N = static_cast<int>(state.range(0));

    TradingEngine  engine(make_config());
    SpreadStrategy strategy(make_strategy_cfg());
    engine.set_strategy(&strategy);

    // Prime: first tick causes order submission (not representative)
    std::ignore = engine.push_tick(make_tick(9998, 10002));
    std::ignore = engine.run_once();

    std::vector<int64_t> samples;
    samples.reserve(static_cast<std::size_t>(N));

    for (auto _ : state) {
        state.PauseTiming();
        samples.clear();
        state.ResumeTiming();

        for (int i = 0; i < N; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            std::ignore = engine.push_tick(make_tick(9998, 10002));
            std::ignore = engine.run_once();
            const auto t1 = std::chrono::steady_clock::now();
            samples.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    }

    std::sort(samples.begin(), samples.end());
    const auto pct = [&](double p) -> int64_t {
        std::size_t idx = static_cast<std::size_t>(p * 0.01 * (N - 1));
        return samples[idx];
    };

    state.SetLabel("N=" + std::to_string(N));
    state.counters["P50_ns"]  = static_cast<double>(pct(50));
    state.counters["P90_ns"]  = static_cast<double>(pct(90));
    state.counters["P99_ns"]  = static_cast<double>(pct(99));
    state.counters["P999_ns"] = static_cast<double>(pct(99.9));
    state.counters["max_ns"]  = static_cast<double>(samples.back());
}
BENCHMARK(BM_EngineLatencyHistogram)->Arg(100000)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
