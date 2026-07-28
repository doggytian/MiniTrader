#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core/spsc_queue.h"
#include "orderbook/order_book.h"
#include "risk/risk_gate.h"
#include "strategy/strategy_base.h"

namespace minitrader {

/// 交易引擎配置。
struct EngineConfig {
    OrderBookConfig book_config{.min_price = 1, .max_price = 100000, .tick_size = 1};
    RiskConfig      risk_config{};
    std::size_t     tick_queue_capacity{4096};  // 仅供参考，队列容量由 kQueueCap 固定
    bool            enable_latency_log{false};  // 是否打印每个 tick 的 on_tick() 耗时
};

/// 交易引擎 — 全链路协调中枢。
///
/// 数据流：
///   MarketTick --> [SPSC 队列] --> run_once()
///                                     --> strategy.on_tick()
///                                         --> strategy.submit_order()
///                                             --> RiskGate.check()
///                                                 --> OrderBook.add_order()
///                                                     --> fill_callback
///                                                         --> strategy.on_fill()
///
/// 线程模型（单线程 demo）：
///   - push_tick()：生产者侧调用（行情线程或测试驱动）
///   - run_once() / run()：消费者侧调用（策略线程）
///   两侧通过 SPSCQueue 通信，热路径零 mutex。
class TradingEngine {
public:
    static constexpr std::size_t kQueueCap = 4096;  // 必须为 2 的幂

    explicit TradingEngine(EngineConfig config = {});

    // 不可拷贝
    TradingEngine(const TradingEngine&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    /// 绑定策略，必须在 run()/run_once() 之前调用。
    /// 引擎会将自身指针注入策略，使 submit_order/cancel_order/position
    /// 的调用自动经过风控和撮合簿路由。
    void set_strategy(StrategyBase* strategy);

    /// 入队一个行情 tick（生产者侧，无锁）。
    /// @return 队列已满（tick 被丢弃）时返回 false。
    [[nodiscard]] bool push_tick(const MarketTick& tick) noexcept;

    /// 同步处理队列中所有待处理 tick（消费者侧）。
    /// 返回本次处理的 tick 数量。
    std::size_t run_once();

    /// 阻塞运行直到 stop() 被调用（消费者侧）。
    void run();

    /// 通知 run() 循环退出（线程安全）。
    void stop() noexcept;

    // ─── 供 StrategyBase 调用（friend）────────────────────────
    /// 经风控网关 → 撮合簿提交订单。
    void submit_order(Order order);

    /// 撤销一笔挂单。
    void cancel_order(uint64_t order_id);

    /// 查询某品种当前净持仓。
    [[nodiscard]] int32_t position(uint64_t instrument_id) const noexcept;

    // ─── 诊断接口 ──────────────────────────────────────────────
    [[nodiscard]] uint64_t ticks_processed() const noexcept { return ticks_processed_; }
    [[nodiscard]] uint64_t orders_submitted() const noexcept { return orders_submitted_; }
    [[nodiscard]] uint64_t orders_rejected() const noexcept { return orders_rejected_; }
    [[nodiscard]] uint64_t fills_received() const noexcept { return fills_received_; }

    /// on_tick() 平均耗时（纳秒），无 tick 时返回 0。
    [[nodiscard]] uint64_t latency_avg_ns() const noexcept {
        return ticks_processed_ ? latency_sum_ns_ / ticks_processed_ : 0;
    }
    /// on_tick() 峰值耗时（纳秒）。
    [[nodiscard]] uint64_t latency_max_ns() const noexcept { return latency_max_ns_; }

    /// 重置所有延迟统计（用于热身轮结束后清零）。
    void reset_latency_stats() noexcept {
        latency_sum_ns_ = 0;
        latency_max_ns_ = 0;
        latency_hist_.fill(0);
        ticks_processed_ = 0;
    }

    // ─── 延迟直方图（on_tick 耗时分布）────────────────────────
    /// 直方图桶上界（纳秒），共 kHistBuckets 个桶：
    ///   [0,100) [100,200) [200,500) [500,1000) [1µs,5µs) [5µs,10µs) [10µs,100µs) [≥100µs]
    static constexpr std::size_t kHistBuckets = 8;
    static constexpr std::array<uint64_t, kHistBuckets - 1> kHistBounds = {
        100, 200, 500, 1'000, 5'000, 10'000, 100'000  // 纳秒上界
    };

    /// 返回各桶计数（只读）。
    [[nodiscard]] const std::array<uint64_t, kHistBuckets>& latency_histogram() const noexcept {
        return latency_hist_;
    }

    /// 计算延迟百分位数（纳秒），基于直方图线性插值。
    /// @param pct 百分比，如 50.0 / 99.0 / 99.9
    /// @return 估算延迟（纳秒），无样本时返回 0
    [[nodiscard]] uint64_t latency_percentile_ns(double pct) const noexcept;

    [[nodiscard]] const OrderBook& order_book() const noexcept { return book_; }
    [[nodiscard]] const RiskGate&  risk_gate()  const noexcept { return risk_; }

private:
    void on_fill(const ExecutionReport& report);

    EngineConfig config_;
    OrderBook    book_;
    RiskGate     risk_;

    SPSCQueue<MarketTick, kQueueCap> tick_queue_;

    StrategyBase* strategy_{nullptr};

    std::atomic<bool> running_{false};

    // 计数器
    uint64_t ticks_processed_{0};
    uint64_t orders_submitted_{0};
    uint64_t orders_rejected_{0};
    uint64_t fills_received_{0};

    // 延迟统计（on_tick 挂钟时间，纳秒）
    uint64_t latency_sum_ns_{0};
    uint64_t latency_max_ns_{0};
    std::array<uint64_t, kHistBuckets> latency_hist_{};
};

}  // namespace minitrader
