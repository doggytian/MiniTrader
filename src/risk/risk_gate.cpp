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
}

void RiskGate::reset() noexcept {
    position_ = 0;
    orders_this_second_ = 0;
    total_orders_ = 0;
    total_cancels_ = 0;
    last_rate_reset_ = std::chrono::steady_clock::now();
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
    double ratio = static_cast<double>(total_cancels_) / static_cast<double>(total_orders_);
    return ratio <= config_.max_cancel_ratio;
}

bool RiskGate::check_order_rate() noexcept {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_rate_reset_);
    if (elapsed.count() >= 1) {
        orders_this_second_ = 0;
        last_rate_reset_ = now;
    }
    orders_this_second_++;
    return orders_this_second_ <= config_.max_orders_per_second;
}

bool RiskGate::check_self_trade(const Order& /*order*/) const noexcept {
    // TODO: Check if this order would match against our own resting order
    // Requires access to the order book's state for our account
    return true;
}

}  // namespace minitrader
