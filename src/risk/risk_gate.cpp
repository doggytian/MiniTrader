#include "risk/risk_gate.h"

namespace minitrader {

RiskResult RiskGate::check(const Order& order) noexcept {
    if (!check_order_rate()) {
        return RiskResult::RejectOrderRate;
    }
    if (!check_position_limit(order)) {
        return RiskResult::RejectPositionLimit;
    }
    if (order.type == OrderType::Cancel && !check_cancel_rate()) {
        return RiskResult::RejectCancelRate;
    }
    if (config_.check_self_trade && !check_self_trade(order)) {
        return RiskResult::RejectSelfTrade;
    }
    total_orders_++;
    return RiskResult::Pass;
}

void RiskGate::on_fill(const ExecutionReport& report) noexcept {
    // 按成交方向更新净持仓
    if (report.side == Side::Buy) {
        position_ += report.filled_quantity;
    } else {
        position_ -= report.filled_quantity;
    }
    // 挂单方（maker）成交后的 untrack 由 TradingEngine 统一处理
}

void RiskGate::reset() noexcept {
    position_ = 0;
    orders_this_second_ = 0;
    total_orders_ = 0;
    total_cancels_ = 0;
    last_rate_reset_ = std::chrono::steady_clock::now();
    active_orders_.clear();
}

void RiskGate::track_order(const Order& order) noexcept {
    active_orders_[order.order_id] = {order.side, order.price};
}

void RiskGate::untrack_order(uint64_t order_id) noexcept {
    active_orders_.erase(order_id);
}

int32_t RiskGate::current_position(uint64_t /*instrument_id*/) const noexcept {
    return position_;  // 当前仅支持单品种
}

bool RiskGate::check_position_limit(const Order& order) const noexcept {
    // 预估成交后的持仓，判断是否超限
    int32_t projected = position_;
    if (order.side == Side::Buy) {
        projected += order.quantity;
    } else {
        projected -= order.quantity;
    }
    return std::abs(projected) <= config_.max_position;
}

bool RiskGate::check_cancel_rate() const noexcept {
    if (total_orders_ == 0) return true;
    double ratio = static_cast<double>(total_cancels_) /
                   static_cast<double>(total_orders_);
    return ratio <= config_.max_cancel_ratio;
}

bool RiskGate::check_order_rate() noexcept {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_rate_reset_);
    if (elapsed.count() >= 1) {
        // 新的一秒，重置本秒计数器
        orders_this_second_ = 0;
        last_rate_reset_ = now;
    }
    orders_this_second_++;
    return orders_this_second_ <= config_.max_orders_per_second;
}

bool RiskGate::check_self_trade(const Order& order) const noexcept {
    // 自成交条件：
    //   进场买单 价格 P，命中己方挂单卖价 <= P
    //   进场卖单 价格 P，命中己方挂单买价 >= P
    if (order.type == OrderType::Market) {
        // 市价单以任意价格成交，只要对侧有挂单即触发
        for (const auto& [id, resting] : active_orders_) {
            if (resting.side != order.side) return false;
        }
        return true;
    }

    for (const auto& [id, resting] : active_orders_) {
        if (resting.side == order.side) continue;  // 同侧，不会交叉
        if (order.side == Side::Buy  && resting.price <= order.price) return false;
        if (order.side == Side::Sell && resting.price >= order.price) return false;
    }
    return true;
}

}  // namespace minitrader
