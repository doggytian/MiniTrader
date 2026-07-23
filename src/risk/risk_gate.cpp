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
    if (report.side == Side::Buy) {
        position_ += report.filled_quantity;
    } else {
        position_ -= report.filled_quantity;
    }
    // Maker side: the resting order was (partially or fully) consumed.
    // Full fill is handled by TradingEngine calling untrack_order().
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
    return position_;
}

bool RiskGate::check_position_limit(const Order& order) const noexcept {
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
        orders_this_second_ = 0;
        last_rate_reset_ = now;
    }
    orders_this_second_++;
    return orders_this_second_ <= config_.max_orders_per_second;
}

bool RiskGate::check_self_trade(const Order& order) const noexcept {
    // A self-trade occurs when an incoming order would cross against one of
    // our own resting orders:
    //   incoming BUY  at price P  would match a resting SELL at price <= P
    //   incoming SELL at price P  would match a resting BUY  at price >= P
    if (order.type == OrderType::Market) {
        // Market orders cross at any price — reject if we have any resting
        // order on the opposite side.
        for (const auto& [id, resting] : active_orders_) {
            if (resting.side != order.side) return false;
        }
        return true;
    }

    for (const auto& [id, resting] : active_orders_) {
        if (resting.side == order.side) continue;  // same side, no cross
        if (order.side == Side::Buy  && resting.price <= order.price) return false;
        if (order.side == Side::Sell && resting.price >= order.price) return false;
    }
    return true;
}

}  // namespace minitrader
