#include <benchmark/benchmark.h>
#include "orderbook/order_book.h"

using namespace minitrader;

// ─── helpers ────────────────────────────────────────────────────────────────

static OrderBookConfig kConfig{.min_price = 1, .max_price = 10000, .tick_size = 1};

static Order make_order(uint64_t& id, Side side, int64_t price, int32_t qty) {
    return Order{id++, 1, price, qty, side, OrderType::Limit, 0, Order::now_ns()};
}

// ─── add_order: continuously add bids at the same price level ───────────────
//
// The book accumulates orders but never overflows (max_price=10000 levels).
// We measure the hot path: price_to_index + list::push_back + map insert.
static void BM_OrderBookAddOrder(benchmark::State& state) {
    OrderBook book(kConfig);
    uint64_t id = 1;

    for (auto _ : state) {
        book.add_order(make_order(id, Side::Buy, 5000, 100));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookAddOrder);

// ─── cancel_order: O(1) via order_map_ ─────────────────────────────────────
//
// Pre-fill N resting orders, then cancel them one-by-one in the timing loop.
// When the pool is exhausted, refill (under PauseTiming).
static void BM_OrderBookCancelOrder(benchmark::State& state) {
    OrderBook book(kConfig);
    uint64_t id = 1;
    const int pool_size = 1024;

    // Pre-fill initial pool
    std::vector<uint64_t> ids;
    ids.reserve(pool_size);
    for (int i = 0; i < pool_size; ++i) {
        Order o = make_order(id, Side::Buy, 5000, 10);
        ids.push_back(o.order_id);
        book.add_order(std::move(o));
    }

    int pos = 0;
    for (auto _ : state) {
        if (pos == pool_size) {
            // Refill outside timing
            state.PauseTiming();
            ids.clear();
            for (int i = 0; i < pool_size; ++i) {
                Order o = make_order(id, Side::Buy, 5000, 10);
                ids.push_back(o.order_id);
                book.add_order(std::move(o));
            }
            pos = 0;
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(book.cancel_order(ids[pos++]));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookCancelOrder);

// ─── match: single incoming order vs N resting orders ──────────────────────
//
// Keep a persistent OrderBook with a fixed pool of resting asks.
// Each iteration: one buy crosses one resting ask (qty=50 vs qty=50 → full fill).
// We replenish under PauseTiming so the book always has liquidity.
static void BM_OrderBookMatch(benchmark::State& state) {
    OrderBook book(kConfig);
    uint64_t id = 1;
    const int resting_depth = static_cast<int>(state.range(0));
    const int64_t match_price = 5000;

    // Prime the book with resting depth
    for (int i = 0; i < resting_depth; ++i) {
        book.add_order(make_order(id, Side::Sell, match_price, 50));
    }

    for (auto _ : state) {
        // Timed: incoming buy triggers one match
        book.add_order(make_order(id, Side::Buy, match_price, 50));

        // Untimed: replenish one ask so depth stays constant
        state.PauseTiming();
        book.add_order(make_order(id, Side::Sell, match_price, 50));
        state.ResumeTiming();
    }
    state.SetLabel("depth=" + std::to_string(resting_depth));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookMatch)->Arg(1)->Arg(10)->Arg(100);

BENCHMARK_MAIN();
