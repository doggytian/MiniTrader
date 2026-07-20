#include <benchmark/benchmark.h>
#include "orderbook/order_book.h"

using namespace minitrader;

static void BM_OrderBookAddOrder(benchmark::State& state) {
    OrderBookConfig config{.min_price = 1, .max_price = 10000, .tick_size = 1};
    OrderBook book(config);
    uint64_t id = 1;

    for (auto _ : state) {
        Order order{
            .order_id = id++,
            .instrument_id = 1,
            .price = 5000,
            .quantity = 100,
            .side = Side::Buy,
            .type = OrderType::Limit,
            .timestamp_ns = Order::now_ns(),
        };
        book.add_order(std::move(order));
    }
}
BENCHMARK(BM_OrderBookAddOrder);

static void BM_OrderBookMatch(benchmark::State& state) {
    OrderBookConfig config{.min_price = 1, .max_price = 10000, .tick_size = 1};
    uint64_t id = 1;

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(config);
        // Pre-fill asks
        for (int i = 0; i < 10; ++i) {
            book.add_order(Order{id++, 1, 5000, 100, Side::Sell, OrderType::Limit, 0, 0});
        }
        state.ResumeTiming();

        // Match against resting asks
        book.add_order(Order{id++, 1, 5000, 50, Side::Buy, OrderType::Limit, 0, Order::now_ns()});
    }
}
BENCHMARK(BM_OrderBookMatch);

BENCHMARK_MAIN();
