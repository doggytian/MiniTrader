#pragma once

#include <cstdint>
#include <string>
#include "strategy/strategy_base.h"

namespace minitrader {

class TradingEngine;  // 供 cancel_resting_quotes 使用的前向声明

/// 价差做市策略配置。
struct SpreadStrategyConfig {
    uint64_t instrument_id{1};
    int64_t  half_spread{2};      // 基础报价半价差（tick 数）
    int32_t  order_size{10};      // 每侧报价数量（手）
    bool     verbose{true};       // 是否打印报价 / 成交日志

    // ── inventory skew ───────────────────────────────────────
    // 每持有 1 手净多头，将报价中间价向下偏移 skew_per_lot tick，
    // 使卖出更容易、买入更难，从而驱动持仓回归零。
    // 净空头时方向相反（中间价上移）。
    int32_t  max_position{20};    // 净持仓绝对值超过此值时暂停双边报价
    double   skew_per_lot{0.3};   // 每手持仓产生的中间价偏移（tick）

    // ── 波动率自适应 spread ───────────────────────────────────
    // 用市场价差（ask-bid）的指数移动平均（EMA）近似短期波动率。
    // 当前市场价差 > ema_spread * spread_scale_threshold 时，
    // 将 half_spread 乘以 spread_scale_factor 加宽，减少逆向选择。
    double   ema_alpha{0.05};              // EMA 平滑系数（越小越平滑）
    double   spread_scale_threshold{1.8};  // 触发加宽的市场价差倍数
    double   spread_scale_factor{1.6};     // 加宽倍率
};

/// 价差做市策略（生产简化版）。
///
/// 在最小骨架基础上增加两项核心机制：
///
/// 1. Inventory Skew（持仓偏斜）
///    净多头时中间价下移 → 报价向卖方倾斜 → 加速平仓；
///    净空头时中间价上移 → 报价向买方倾斜 → 加速平仓。
///    净持仓超过 max_position 时暂停新报价，等待成交降仓。
///
/// 2. Volatility-Adaptive Spread（波动率自适应价差）
///    监控市场实时价差的 EMA，市场价差突然扩大（逆向选择风险上升）
///    时自动加宽我方报价区间，牺牲成交频率换取被套概率降低。
///
/// 仍未包含的生产级细节：多档挂单、信号过滤、手续费建模、
/// 滑点/latency 补偿、动态调整 order_size 等。
class SpreadStrategy : public StrategyBase {
public:
    explicit SpreadStrategy(SpreadStrategyConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] std::string name() const override { return "SpreadStrategy"; }

    void on_start() override;
    void on_tick(const MarketTick& tick) override;
    void on_fill(const ExecutionReport& report) override;
    void on_stop() override;

    /// 撤销当前活跃报价并重置内部 ID（供 benchmark / 清仓使用）。
    void cancel_resting_quotes(TradingEngine& engine);

private:
    uint64_t next_order_id() noexcept { return ++next_id_; }

    SpreadStrategyConfig cfg_;
    uint64_t next_id_{0};

    // 当前活跃报价的订单 ID（0 = 无挂单）
    uint64_t bid_order_id_{0};
    uint64_t ask_order_id_{0};

    // 盈亏跟踪（tick × 手，非真实货币）
    int64_t  realized_pnl_{0};
    int32_t  fill_count_{0};

    // 波动率代理：市场价差（ask-bid）的 EMA
    double   ema_spread_{0.0};   // 首个 tick 直接初始化
    bool     ema_initialized_{false};
};

}  // namespace minitrader
