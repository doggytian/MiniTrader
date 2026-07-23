#pragma once

#include <cstdint>
#include <string>
#include "strategy/strategy_base.h"

namespace minitrader {

class TradingEngine;  // 供 cancel_resting_quotes 使用的前向声明

/// 价差做市策略配置。
struct SpreadStrategyConfig {
    uint64_t instrument_id{1};
    int64_t  half_spread{1};   // 报价偏离中间价的 tick 数（单侧）
    int32_t  order_size{10};   // 每侧报价数量（手）
    bool     verbose{true};    // 是否打印报价 / 成交日志
};

/// 简单价差做市策略。
///
/// 逻辑：
///   每个 tick，若双侧均无挂单：
///     - 在 (mid - half_spread) 挂买单
///     - 在 (mid + half_spread) 挂卖单
///   成交后：
///     - 撤销未成交的对侧单（防止单侧持仓积累）
///     - 等待下一个 tick 重新双边报价
///
/// 这是做市策略的最小骨架，刻意省略了：
/// 持仓倾斜（inventory skew）、逆向选择过滤、手续费建模等生产级细节。
class SpreadStrategy : public StrategyBase {
public:
    explicit SpreadStrategy(SpreadStrategyConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] std::string name() const override { return "SpreadStrategy"; }

    void on_start() override;
    void on_tick(const MarketTick& tick) override;
    void on_fill(const ExecutionReport& report) override;
    void on_stop() override;

    /// 撤销当前活跃报价并重置内部 ID（供 benchmark 使用）。
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
};

}  // namespace minitrader
