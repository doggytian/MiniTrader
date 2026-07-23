#include <cstdio>
#include <cstdint>
#include <thread>
#include <chrono>

#include "engine/trading_engine.h"
#include "strategy/spread_strategy.h"

using namespace minitrader;

int main() {
    std::printf("=== MiniTrader Demo ===\n\n");

    // ── Engine setup ─────────────────────────────────────────────────────────
    EngineConfig eng_cfg;
    eng_cfg.book_config = {.min_price = 1, .max_price = 100000, .tick_size = 1};
    eng_cfg.risk_config = {
        .max_position       = 500,
        .max_orders_per_second = 200,
        .max_cancel_ratio   = 0.9,
        .check_self_trade   = false,  // disabled: strategy trades with itself in demo
    };

    TradingEngine engine(eng_cfg);

    // ── Strategy setup ───────────────────────────────────────────────────────
    SpreadStrategy strategy(SpreadStrategyConfig{
        .instrument_id = 1,
        .half_spread   = 2,   // quote 2 ticks each side of mid
        .order_size    = 5,
    });

    engine.set_strategy(&strategy);

    // ── Mock market data ─────────────────────────────────────────────────────
    // Simulate 20 ticks: price drifts up then down, spread narrows once.
    // Occasionally an aggressive order appears and crosses our quote.
    struct MockTick { int64_t bid; int64_t ask; };
    const MockTick ticks[] = {
        {9995, 10005},   // wide spread, mid=10000
        {9996, 10004},
        {9997, 10003},
        {9998, 10002},   // narrow — our ask at 10001 may get hit externally
        {9999, 10001},   // very tight; mid=10000, we quote 9998/10002
        {9998, 10002},
        {9997, 10003},
        {9995, 10005},
        {9990, 10010},   // sudden widen
        {9992, 10008},
        {9994, 10006},
        {9996, 10004},
        {9998, 10002},
        {10000, 10004},  // bid jumps — someone buys aggressively
        {10001, 10005},
        {10000, 10004},
        {9999, 10003},
        {9998, 10002},
        {9997, 10001},
        {9996, 10000},
    };

    std::printf("── Feeding %zu ticks ──\n\n", std::size(ticks));

    for (std::size_t i = 0; i < std::size(ticks); ++i) {
        const auto& t = ticks[i];
        MarketTick tick{
            .instrument_id        = 1,
            .bid_price            = t.bid,
            .ask_price            = t.ask,
            .bid_size             = 100,
            .ask_size             = 100,
            .last_price           = (t.bid + t.ask) / 2,
            .last_size            = 10,
            .exchange_timestamp_ns = Order::now_ns(),
            .local_timestamp_ns    = Order::now_ns(),
        };

        engine.push_tick(tick);
        engine.run_once();

        // After tick 2: external aggressive sell hits our resting bid
        // After tick 8: external aggressive buy hits our resting ask
        if (i == 2) {
            std::printf("[external] aggressive SELL @ 9998 hits our bid\n");
            engine.submit_order(Order{
                9000001, 1, 9998, 5, Side::Sell, OrderType::Limit, 0, Order::now_ns()
            });
        }
        if (i == 8) {
            std::printf("[external] aggressive BUY @ 10002 hits our ask\n");
            engine.submit_order(Order{
                9000002, 1, 10002, 5, Side::Buy, OrderType::Limit, 0, Order::now_ns()
            });
        }
    }

    // ── Force strategy shutdown ──────────────────────────────────────────────
    strategy.on_stop();

    // ── Summary ─────────────────────────────────────────────────────────────
    std::printf("\n── Engine stats ──\n");
    std::printf("  ticks processed : %llu\n",
                static_cast<unsigned long long>(engine.ticks_processed()));
    std::printf("  orders submitted: %llu\n",
                static_cast<unsigned long long>(engine.orders_submitted()));
    std::printf("  orders rejected : %llu\n",
                static_cast<unsigned long long>(engine.orders_rejected()));
    std::printf("  fills received  : %llu\n",
                static_cast<unsigned long long>(engine.fills_received()));
    std::printf("  best bid        : %lld\n",
                static_cast<long long>(engine.order_book().best_bid()));
    std::printf("  best ask        : %lld\n",
                static_cast<long long>(engine.order_book().best_ask()));

    return 0;
}
