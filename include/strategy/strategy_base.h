#pragma once

#include <string>
#include "orderbook/order.h"

namespace minitrader {

// 前向声明，避免循环 include
class TradingEngine;

/// 行情 tick 事件。
struct MarketTick {
    uint64_t instrument_id;          // 品种 / 合约 ID
    int64_t  bid_price;              // 最优买价
    int64_t  ask_price;              // 最优卖价
    int32_t  bid_size;               // 买一量
    int32_t  ask_size;               // 卖一量
    int64_t  last_price;             // 最新成交价
    int32_t  last_size;              // 最新成交量
    uint64_t exchange_timestamp_ns;  // 交易所时间戳（纳秒）
    uint64_t local_timestamp_ns;     // 本地接收时间戳（纳秒）
};

/// 策略抽象基类（Template Method 模式）。
///
/// 派生策略只需实现 on_tick() 和 on_fill()。
/// 订单路由和风控检查由框架透明处理——
/// 策略代码调用 submit_order() / cancel_order() / position()，
/// 无需直接接触 OrderBook 或 RiskGate。
class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    /// 每个行情 tick 到达时调用。
    virtual void on_tick(const MarketTick& tick) = 0;

    /// 订单成交时调用（买卖双方各收一条回报）。
    virtual void on_fill(const ExecutionReport& report) = 0;

    /// 策略启动时调用一次（初始化状态、预加载数据）。
    virtual void on_start() {}

    /// 策略停止时调用一次（清理、汇报盈亏）。
    virtual void on_stop() {}

    /// 策略名称（用于日志标识）。
    [[nodiscard]] virtual std::string name() const = 0;

protected:
    /// 提交新订单，经风控网关 → 撮合簿路由。
    /// 被风控拒绝的订单静默丢弃（可通过引擎诊断接口查看）。
    void submit_order(Order order);

    /// 撤销一笔挂单。
    void cancel_order(uint64_t order_id);

    /// 查询某品种净持仓（正数 = 多头，负数 = 空头）。
    [[nodiscard]] int32_t position(uint64_t instrument_id) const;

private:
    friend class TradingEngine;
    TradingEngine* engine_{nullptr};  // 由 TradingEngine::set_strategy() 注入
};

}  // namespace minitrader
