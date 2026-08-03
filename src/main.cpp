#include <atomic>
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

#include "core/thread_utils.h"
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

    // 单线程全速回放：主线程同时承担生产者+消费者，绑到 core 0 减少跨核迁移
    log_pin_result("recv(main)", 0, pin_current_thread_to_core(0));

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

    // 第 1~ROUNDS-1 轮：正式测量。
    // 每处理 N 个 tick 注入一个外部激进单（交叉 mid ± half_spread），
    // 触发策略挂单成交 → on_fill → cancel 对侧单，让延迟路径分化。
    constexpr int INJECT_EVERY = 18;  // ~5.5% 的 tick 后注入，模拟偶尔的市场冲击
    uint64_t inject_id = 9000000;

    for (int r = 1; r < ROUNDS; ++r) {
        int tick_idx = 0;
        for (const auto& tick : ticks) {
            // 正常 tick 处理（报价 / 检查挂单状态）
            std::ignore = engine.push_tick(tick);
            std::ignore = engine.run_once();

            // 每隔 N tick 注外部激进单，模拟瞬间市场冲击
            if (++tick_idx % INJECT_EVERY == 0) {
                const int64_t mid = (tick.bid_price + tick.ask_price) / 2;
                // 交替方向：买方冲击（打 sell）还是卖方冲击（打 buy）
                if (r % 2 == 0) {
                    // 外部激进买单 @ mid+2 → 打到策略的挂卖单
                    engine.submit_order(Order{
                        ++inject_id, tick.instrument_id,
                        mid + 2, 5, Side::Buy,
                        OrderType::Limit, 0, Order::now_ns()});
                } else {
                    // 外部激进卖单 @ mid-2 → 打到策略的挂买单
                    engine.submit_order(Order{
                        ++inject_id, tick.instrument_id,
                        mid - 2, 5, Side::Sell,
                        OrderType::Limit, 0, Order::now_ns()});
                }
            }
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
        "<25ns",  "<50ns", "<75ns", "<100ns",
        "<200ns", "<500ns", "<1µs",
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

// ── 真实速率回放模式 ──────────────────────────────────────────────────────────
// 用法：./minitrader_demo --realtime <csv文件路径> [speed_multiplier]
//
// 设计目标：测量"真实报文速率下的排队延迟（背压）"，弥补全速回放只测逻辑吞吐上限的盲区。
//
// 架构：
//   生产者线程（producer）：按 CSV exchange_timestamp_ns 间隔 sleep_until，push_tick；
//                           队列满时 ticks_dropped++ 并继续（不阻塞）。
//   消费者（主线程）       ：持续 run_once() 排空队列，同时计逻辑延迟 + 排队延迟。
//
// 关键指标：
//   on_tick 逻辑延迟  ─ 与全速回放一致（出队→策略返回），反映纯业务路径
//   排队延迟（queue wait） ─ 出队时刻 - 入队时刻，反映背压/积压
//   丢包率            ─ ticks_dropped / total_sent，超 0.1% 说明消费跟不上
//
// speed_multiplier > 1 加速（2 = 2× 速率，压测用）；默认 1.0（原速）。
static void run_realtime_mode(const char* csv_path, double speed = 1.0) {
    using Clock = std::chrono::steady_clock;

    std::printf("=== MiniTrader 真实速率回放（speed=%.1fx）===\n", speed);
    std::printf("文件：%s\n\n", csv_path);

    // 双线程绑核：消费者（主线程）→ core 1，生产者 → core 0
    // 两者绑在不同核上，消除互相抢占；同在一个 NUMA node 保证 SPSC cacheline 传输最短路径
    log_pin_result("consumer(main)", 1, pin_current_thread_to_core(1));

    const auto ticks = load_csv(csv_path);
    if (ticks.size() < 2) {
        std::fprintf(stderr, "[realtime] CSV 不足 2 条 tick，退出。\n");
        return;
    }
    std::printf("已预读 %zu 条 tick\n", ticks.size());

    // ── 引擎 & 策略 ───────────────────────────────────────────────────────────
    EngineConfig eng_cfg;
    eng_cfg.book_config = {.min_price = 1, .max_price = 100000, .tick_size = 1};
    eng_cfg.risk_config = {.max_position = 500, .max_orders_per_second = 200,
                           .max_cancel_ratio = 0.9, .check_self_trade = false};
    TradingEngine engine(eng_cfg);
    engine.enable_queue_latency_tracking(true);

    SpreadStrategy strategy(SpreadStrategyConfig{
        .instrument_id = 1, .half_spread = 2, .order_size = 5, .verbose = false,
    });
    engine.set_strategy(&strategy);
    strategy.on_start();

    // ── 生产者线程：按时间戳节流 push ────────────────────────────────────────
    std::atomic<bool> producer_done{false};
    uint64_t total_sent = 0;

    std::thread producer([&] {
        // 生产者绑到 core 0（与消费者 core 1 相邻但独立，SPSC cacheline 跨核传输一跳）
        log_pin_result("producer", 0, pin_current_thread_to_core(0));

        // 找第一个有效时间戳作为基准
        const uint64_t first_ts = ticks[0].exchange_timestamp_ns;
        const auto     wall0    = Clock::now();

        for (std::size_t i = 0; i < ticks.size(); ++i) {
            // 按时间戳计算应推进的目标时刻（speed > 1 则压缩间隔）
            const uint64_t tick_ts = ticks[i].exchange_timestamp_ns;
            const uint64_t offset_ns = (tick_ts >= first_ts)
                ? static_cast<uint64_t>((tick_ts - first_ts) / speed)
                : 0;
            const auto target = wall0 + std::chrono::nanoseconds(offset_ns);
            std::this_thread::sleep_until(target);

            engine.push_tick(ticks[i]);  // 失败时内部 ticks_dropped_++
            ++total_sent;
        }
        producer_done.store(true, std::memory_order_release);
    });

    // ── 消费者（主线程）：持续排空队列 ───────────────────────────────────────
    while (!producer_done.load(std::memory_order_acquire)
           || !engine.tick_queue_empty()) {
        engine.run_once();
    }
    // 最后再排一次，确保 producer 退出后残留的 tick 全处理完
    engine.run_once();

    producer.join();
    strategy.on_stop();

    // ── 报告 ─────────────────────────────────────────────────────────────────
    const uint64_t processed = engine.ticks_processed();
    const uint64_t dropped   = engine.ticks_dropped();
    const double   drop_rate = total_sent ? 100.0 * dropped / total_sent : 0.0;

    std::printf("\n── 吞吐统计 ──\n");
    std::printf("  发送 tick 数   : %llu\n", static_cast<unsigned long long>(total_sent));
    std::printf("  处理 tick 数   : %llu\n", static_cast<unsigned long long>(processed));
    std::printf("  丢弃 tick 数   : %llu  (丢包率 %.4f%%)\n",
                static_cast<unsigned long long>(dropped), drop_rate);
    std::printf("  已提交订单数   : %llu\n",
                static_cast<unsigned long long>(engine.orders_submitted()));

    // ── 排队延迟（背压指标）──────────────────────────────────────────────────
    std::printf("\n── 排队延迟（入队→出队，反映背压）──\n");
    std::printf("  均值   : %6llu ns\n",
                static_cast<unsigned long long>(engine.queue_wait_avg_ns()));
    std::printf("  峰值   : %6llu ns\n",
                static_cast<unsigned long long>(engine.queue_wait_max_ns()));
    std::printf("  P50    : %6llu ns\n",
                static_cast<unsigned long long>(engine.queue_wait_percentile_ns(50.0)));
    std::printf("  P99    : %6llu ns\n",
                static_cast<unsigned long long>(engine.queue_wait_percentile_ns(99.0)));
    std::printf("  P99.9  : %6llu ns\n",
                static_cast<unsigned long long>(engine.queue_wait_percentile_ns(99.9)));

    static constexpr const char* kQLabels[TradingEngine::kQHistBuckets] = {
        "<1µs", "<5µs", "<10µs", "<50µs", "<100µs", "<500µs", "<1ms", "≥1ms",
    };
    std::printf("\n── 排队延迟直方图 ──\n");
    for (std::size_t b = 0; b < TradingEngine::kQHistBuckets; ++b) {
        const auto& qh = engine.queue_wait_histogram();
        const double pct = processed ? 100.0 * qh[b] / processed : 0.0;
        const int bar = static_cast<int>(pct / 100.0 * 40);
        std::printf("  %-8s %6llu (%5.1f%%)  |",
                    kQLabels[b], static_cast<unsigned long long>(qh[b]), pct);
        for (int i = 0; i < bar; ++i) std::putchar('#');
        std::putchar('\n');
    }

    // ── on_tick 逻辑延迟（与全速回放对比用）────────────────────────────────
    std::printf("\n── on_tick 逻辑延迟（出队→策略返回）──\n");
    std::printf("  均值   : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_avg_ns()));
    std::printf("  P50    : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_percentile_ns(50.0)));
    std::printf("  P99    : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_percentile_ns(99.0)));
    std::printf("  P99.9  : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_percentile_ns(99.9)));
    std::printf("  峰值   : %6llu ns\n",
                static_cast<unsigned long long>(engine.latency_max_ns()));

    if (drop_rate > 0.1)
        std::printf("\n[警告] 丢包率 %.4f%% > 0.1%%，消费者跟不上此速率。\n", drop_rate);
    else
        std::printf("\n[OK] 丢包率 %.4f%%，消费者可跟上此速率。\n", drop_rate);
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
// 默认 mock 模式：    ./minitrader_demo
// 全速回放模式：      ./minitrader_demo --recv data/sample_ticks.csv
// 真实速率回放模式：  ./minitrader_demo --realtime data/sample_ticks.csv [speed]
//   speed 默认 1.0（原速），speed=2 表示 2× 速率压测
int main(int argc, char* argv[]) {
    if (argc >= 3 && std::strcmp(argv[1], "--recv") == 0) {
        run_recv_mode(argv[2]);
    } else if (argc >= 3 && std::strcmp(argv[1], "--realtime") == 0) {
        const double speed = (argc >= 4) ? std::atof(argv[3]) : 1.0;
        run_realtime_mode(argv[2], speed > 0.0 ? speed : 1.0);
    } else {
        run_mock_mode();
    }
    return 0;
}
