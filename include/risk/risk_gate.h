#pragma once

#include <cstdint>
#include <chrono>
#include <unordered_map>
#include "orderbook/order.h"

namespace minitrader {

/// 风控检查结果。
enum class RiskResult : uint8_t {
    Pass = 0,
    RejectCancelRate,    // 撤单率超限
    RejectSelfTrade,     // 会与自己的挂单成交（自成交检测）
    RejectPositionLimit, // 持仓超限
    RejectOrderRate,     // 每秒下单频率超限
};

/// 风控网关配置。
struct RiskConfig {
    int32_t max_position{1000};          // 单品种最大绝对持仓
    int32_t max_orders_per_second{100};  // 每秒最大下单数
    double  max_cancel_ratio{0.8};       // 撤单率上限（证监会规则）
    bool    check_self_trade{true};      // 是否启用自成交检测
};

/// 内联风控网关，位于策略与订单网关之间的关键路径上。
///
/// 设计原则：
/// - 热路径零堆分配（unordered_map 预先分配）
/// - 所有检查均为 O(1)
/// - 不可绕过：策略必须通过风控才能下单
class RiskGate {
public:
    explicit RiskGate(RiskConfig config) : config_(config) {}

    /// 对一笔订单执行全部风控检查。
    [[nodiscard]] RiskResult check(const Order& order) noexcept;

    /// 成交后更新内部状态（持仓跟踪）。
    void on_fill(const ExecutionReport& report) noexcept;

    /// 重置所有计数器（如每个交易日开始时调用）。
    void reset() noexcept;

    /// 登记一笔新挂单，用于自成交检测。
    /// 由 TradingEngine 在限价单进入订单簿后调用。
    void track_order(const Order& order) noexcept;

    /// 注销一笔挂单（撤单或完全成交时调用）。
    void untrack_order(uint64_t order_id) noexcept;

    // ─── 诊断接口 ────────────────────────────────────────────
    [[nodiscard]] int32_t current_position(uint64_t instrument_id) const noexcept;
    [[nodiscard]] int32_t total_orders_today() const noexcept { return total_orders_; }
    [[nodiscard]] int32_t total_cancels_today() const noexcept { return total_cancels_; }

private:
    [[nodiscard]] bool check_position_limit(const Order& order) const noexcept;
    [[nodiscard]] bool check_cancel_rate() const noexcept;
    [[nodiscard]] bool check_order_rate() noexcept;

    /// 检查进场单是否会与自己的挂单成交（自成交检测）。
    /// O(N_active)：遍历活跃挂单检查价格交叉。
    [[nodiscard]] bool check_self_trade(const Order& order) const noexcept;

    RiskConfig config_;

    int32_t position_{0};              // 当前净持仓

    int32_t orders_this_second_{0};    // 本秒已下单数
    int32_t total_orders_{0};          // 今日累计下单数
    int32_t total_cancels_{0};         // 今日累计撤单数
    std::chrono::steady_clock::time_point last_rate_reset_;

    /// 活跃挂单表：order_id → (方向, 价格)。
    /// 用于 O(1) 自成交检测，不依赖订单簿查询。
    struct RestingOrder { Side side; int64_t price; };
    std::unordered_map<uint64_t, RestingOrder> active_orders_;
};

}  // namespace minitrader
