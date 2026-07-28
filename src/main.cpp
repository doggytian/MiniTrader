#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <thread>
#include <chrono>
#include <vector>

#include "engine/trading_engine.h"
#include "network/market_receiver.h"
#include "orderbook/order.h"
#include "strategy/spread_strategy.h"

using namespace minitrader;

// ── CSV 预读：将 CSV 解析为 tick 列表（与 MarketReceiver 格式兼容）──────────
static std::vector<MarketTick> load_csv(const char* path) {
    std::vector<MarketTick> ticks;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[replay] 无法打开文件：%s\n", path);
        return ticks;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line[0] < '0' || line[0] > '9') continue;  // 跳过表头
        std::istringstream ss(line);
        MarketTick tick{};
        char comma;
        if (!(ss >> tick.instrument_id >> comma
                 >> tick.bid_price    >> comma
                 >> tick.ask_price    >> comma
                 >> tick.bid_size     >> comma
                 >> tick.ask_size     >> comma
                 >> tick.last_price   >> comma
                 >> tick.last_size)) continue;
        uint64_t ts = 0; char c2 = 0;
        tick.exchange_timestamp_ns = (ss >> c2 >> ts) ? ts : 0;
        ticks.push_back(tick);
    }
    return ticks;
}

// ── CSV 回放模式 ──────────────────────────────────────────────────────────────
// 用法：./minitrader_demo --recv <csv文件路径>
//
// 两阶段设计：
//   1. 热身（第 1 轮）：策略建仓、icache/dcache 预热，不计入统计。
//   2. 测量（后续 ROUNDS-1 轮）：纯业务路径，无 sleep，保证足够样本量。
//      99 行 × 99 轮 ≈ 9801 次，P99 = 第 9703 名，P99.9 = 第 9791 名，统计可信。
//
// 注意：多轮回放不再 sleep（tick 间隔已在 MarketReceiver 场景中验证），
// 这里测量的是业务逻辑本身的 on_tick 耗时，不混入网络/OS 调度延迟。
static void run_recv_mode(const char* csv_path) {
    constexpr int ROUNDS = 100;  // 第 0 轮热身，第 1~99 轮计入统计

    std::printf("=== MiniTrader CSV 回放模式（%d 轮，统计 %d 轮）===\n",
                ROUNDS, ROUNDS - 1);
    std::printf("文件：%s\n\n", csv_path);

    // 预读全部 tick
    const auto ticks = load_csv(csv_path);
    if (ticks.empty()) {
        std::fprintf(stderr, "[replay] CSV 为空或解析失败，退出。\n");
        return;
    }
    std::printf("已预读 %zu 条 tick，开始 %d 轮回放...\n\n",
                ticks.size(), ROUNDS);

    EngineConfig eng_cfg;
    eng_cfg.book_config        = {.min_price = 1, .max_price = 100000, .tick_size = 1};
    eng_cfg.risk_config        = {.max_position = 500, .max_orders_per_second = 200,
                                  .max_cancel_ratio = 0.9, .check_self_trade = false};
    eng_cfg.enable_latency_log = false;

    TradingEngine engine(eng_cfg);

    SpreadStrategy strategy(SpreadStrategyConfig{
        .instrument_id = 1,
        .half_spread   = 2,
        .order_size    = 5,
        .verbose       = false,  // 多轮回放关闭逐条日志
    });
    engine.set_strategy(&strategy);
    strategy.on_start();

    // 第 0 轮：热身，不计入延迟统计（让 icache/branch predictor 预热）
    for (const auto& tick : ticks) {
        std::ignore = engine.push_tick(tick);
        std::ignore = engine.run_once();
    }

    // 重置延迟统计（热身数据丢弃）
    engine.reset_latency_stats();

    // 第 1~ROUNDS-1 轮：正式测量
    for (int r = 1; r < ROUNDS; ++r) {
        for (const auto& tick : ticks) {
            std::ignore = engine.push_tick(tick);
            std::ignore = engine.run_once();
        }
    }

    strategy.on_stop();

    // ── 引擎统计 ──────────────────────────────────────────────────────────────
    std::printf("\n── 引擎统计（%d 轮 × %zu tick = %llu 次调用）──\n",
                ROUNDS - 1, ticks.size(),
                static_cast<unsigned long long>(engine.ticks_processed()));
    std::printf("  已提交订单数   : %llu\n",
                static_cast<unsigned long long>(engine.orders_submitted()));
    std::printf("  已收到成交数   : %llu\n",
                static_cast<unsigned long long>(engine.fills_received()));

    // ── on_tick() 延迟报告 ────────────────────────────────────────────────────
    std::printf("\n── on_tick() 延迟分布（%llu 次样本，热身 1 轮已丢弃）──\n",
                static_cast<unsigned long long>(engine.ticks_processed()));
    std::printf("  均值   : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_avg_ns()));
    std::printf("  峰值   : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_max_ns()));
    std::printf("  P50    : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_percentile_ns(50.0)));
    std::printf("  P99    : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_percentile_ns(99.0)));
    std::printf("  P99.9  : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_percentile_ns(99.9)));

    // 直方图
    std::printf("\n── 延迟直方图 ──\n");
    const auto& hist = engine.latency_histogram();
    const uint64_t total = engine.ticks_processed();
    static constexpr const char* kLabels[TradingEngine::kHistBuckets] = {
        "<100ns", "<200ns", "<500ns", "<1µs",
        "<5µs",   "<10µs",  "<100µs", "≥100µs",
    };
    for (std::size_t b = 0; b < TradingEngine::kHistBuckets; ++b) {
        const double pct = total ? 100.0 * hist[b] / total : 0.0;
        // 简易 ASCII 进度条（最宽 40 列）
        const int bar_len = static_cast<int>(pct / 100.0 * 40);
        std::printf("  %-8s %5llu (%5.1f%%)  |",
                    kLabels[b],
                    static_cast<unsigned long long>(hist[b]),
                    pct);
        for (int i = 0; i < bar_len; ++i) std::putchar('#');
        std::putchar('\n');
    }
}

// ── Mock 行情模式（默认）─────────────────────────────────────────────────────
static void run_mock_mode() {
    EngineConfig eng_cfg;
    eng_cfg.book_config = {.min_price = 1, .max_price = 100000, .tick_size = 1};
    eng_cfg.risk_config = {
        .max_position          = 500,
        .max_orders_per_second = 200,
        .max_cancel_ratio      = 0.9,
        .check_self_trade      = false,  // demo 中策略与自己撮合，关闭自成交检测
    };
    eng_cfg.enable_latency_log = true;  // 打印每个 tick 的 on_tick 耗时

    TradingEngine engine(eng_cfg);

    // ── 策略配置 ─────────────────────────────────────────────────────────────
    SpreadStrategy strategy(SpreadStrategyConfig{
        .instrument_id = 1,
        .half_spread   = 2,  // 在中间价两侧各 2 tick 报价
        .order_size    = 5,
    });

    engine.set_strategy(&strategy);

    // ── 模拟行情数据 ──────────────────────────────────────────────────────────
    // 20 个 tick：价格先涨后跌，价差时宽时窄。
    // 其中两个时刻有外部激进单主动成交我们的挂单。
    struct MockTick { int64_t bid; int64_t ask; };
    const MockTick ticks[] = {
        {9995, 10005},   // 价差宽，mid=10000
        {9996, 10004},
        {9997, 10003},
        {9998, 10002},
        {9999, 10001},   // 价差极窄
        {9998, 10002},
        {9997, 10003},
        {9995, 10005},
        {9990, 10010},   // 价差突然扩大
        {9992, 10008},
        {9994, 10006},
        {9996, 10004},
        {9998, 10002},
        {10000, 10004},  // 买一上移，有人主动买入
        {10001, 10005},
        {10000, 10004},
        {9999, 10003},
        {9998, 10002},
        {9997, 10001},
        {9996, 10000},
    };

    std::printf("── 推送 %zu 个 tick ──\n\n", std::size(ticks));

    for (std::size_t i = 0; i < std::size(ticks); ++i) {
        const auto& t = ticks[i];
        MarketTick tick{
            .instrument_id         = 1,
            .bid_price             = t.bid,
            .ask_price             = t.ask,
            .bid_size              = 100,
            .ask_size              = 100,
            .last_price            = (t.bid + t.ask) / 2,
            .last_size             = 10,
            .exchange_timestamp_ns = Order::now_ns(),
            .local_timestamp_ns    = Order::now_ns(),
        };

        std::ignore = engine.push_tick(tick);
        std::ignore = engine.run_once();

        // tick 2 之后：外部激进卖单打到我们的买价
        // tick 8 之后：外部激进买单打到我们的卖价
        if (i == 2) {
            std::printf("[外部] 激进卖单 @ 9998 打到我们的买单\n");
            engine.submit_order(Order{
                9000001, 1, 9998, 5, Side::Sell, OrderType::Limit, 0, Order::now_ns()
            });
        }
        if (i == 8) {
            std::printf("[外部] 激进买单 @ 10002 打到我们的卖单\n");
            engine.submit_order(Order{
                9000002, 1, 10002, 5, Side::Buy, OrderType::Limit, 0, Order::now_ns()
            });
        }
    }

    // ── 策略停止 ──────────────────────────────────────────────────────────────
    strategy.on_stop();

    // ── 统计摘要 ──────────────────────────────────────────────────────────────
    std::printf("\n── 引擎统计 ──\n");
    std::printf("  已处理 tick 数 : %llu\n",
                static_cast<unsigned long long>(engine.ticks_processed()));
    std::printf("  已提交订单数   : %llu\n",
                static_cast<unsigned long long>(engine.orders_submitted()));
    std::printf("  被拒绝订单数   : %llu\n",
                static_cast<unsigned long long>(engine.orders_rejected()));
    std::printf("  已收到成交数   : %llu\n",
                static_cast<unsigned long long>(engine.fills_received()));
    std::printf("  当前最优买价   : %lld\n",
                static_cast<long long>(engine.order_book().best_bid()));
    std::printf("  当前最优卖价   : %lld\n",
                static_cast<long long>(engine.order_book().best_ask()));
    std::printf("\n── on_tick() 延迟（出队 → 策略返回）──\n");
    std::printf("  均值 : %llu ns\n",
                static_cast<unsigned long long>(engine.latency_avg_ns()));
    std::printf("  峰值 : %llu ns\n",
                static_cast<unsigned long long>(engine.latency_max_ns()));
}

// ── 入口 ──────────────────────────────────────────────────────────────────────
// 默认 mock 模式：./minitrader_demo
// CSV 回放模式：  ./minitrader_demo --recv data/sample_ticks.csv
int main(int argc, char* argv[]) {
    if (argc >= 3 && std::strcmp(argv[1], "--recv") == 0) {
        run_recv_mode(argv[2]);
    } else {
        run_mock_mode();
    }
    return 0;
}
