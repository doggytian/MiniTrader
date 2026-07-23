#include "strategy/spread_strategy.h"

#include <cstdio>

namespace minitrader {

void SpreadStrategy::on_start() {
    std::printf("[%s] started  half_spread=%lld  size=%d\n",
                name().c_str(),
                static_cast<long long>(cfg_.half_spread),
                cfg_.order_size);
}

void SpreadStrategy::on_tick(const MarketTick& tick) {
    if (tick.instrument_id != cfg_.instrument_id) return;
    if (tick.bid_price <= 0 || tick.ask_price <= 0) return;

    // Only re-quote when both sides are flat (no live orders)
    if (bid_order_id_ != 0 || ask_order_id_ != 0) return;

    const int64_t mid  = (tick.bid_price + tick.ask_price) / 2;
    const int64_t buy_px  = mid - cfg_.half_spread;
    const int64_t sell_px = mid + cfg_.half_spread;

    if (buy_px <= 0 || sell_px <= buy_px) return;

    // Submit bid
    const uint64_t bid_id = next_order_id();
    submit_order(Order{
        .order_id      = bid_id,
        .instrument_id = cfg_.instrument_id,
        .price         = buy_px,
        .quantity      = cfg_.order_size,
        .side          = Side::Buy,
        .type          = OrderType::Limit,
        .timestamp_ns  = Order::now_ns(),
    });

    // Submit ask
    const uint64_t ask_id = next_order_id();
    submit_order(Order{
        .order_id      = ask_id,
        .instrument_id = cfg_.instrument_id,
        .price         = sell_px,
        .quantity      = cfg_.order_size,
        .side          = Side::Sell,
        .type          = OrderType::Limit,
        .timestamp_ns  = Order::now_ns(),
    });

    bid_order_id_ = bid_id;
    ask_order_id_ = ask_id;

    std::printf("[%s] quoted  bid=%lld  ask=%lld  pos=%d\n",
                name().c_str(),
                static_cast<long long>(buy_px),
                static_cast<long long>(sell_px),
                position(cfg_.instrument_id));
}

void SpreadStrategy::on_fill(const ExecutionReport& report) {
    ++fill_count_;

    const bool filled_bid = (report.order_id == bid_order_id_);
    const bool filled_ask = (report.order_id == ask_order_id_);

    if (filled_bid) {
        // Bought: cancel the unfilled ask to avoid one-sided inventory
        if (ask_order_id_ != 0) {
            cancel_order(ask_order_id_);
            ask_order_id_ = 0;
        }
        bid_order_id_ = 0;
        // P&L contribution: will realize when the paired sell fills
        realized_pnl_ -= report.price * report.filled_quantity;
    } else if (filled_ask) {
        if (bid_order_id_ != 0) {
            cancel_order(bid_order_id_);
            bid_order_id_ = 0;
        }
        ask_order_id_ = 0;
        realized_pnl_ += report.price * report.filled_quantity;
    }

    std::printf("[%s] fill  side=%s  price=%lld  qty=%d  pos=%d  fills=%d\n",
                name().c_str(),
                report.side == Side::Buy ? "BUY" : "SELL",
                static_cast<long long>(report.price),
                report.filled_quantity,
                position(cfg_.instrument_id),
                fill_count_);
}

void SpreadStrategy::on_stop() {
    std::printf("[%s] stopped  fills=%d  pos=%d  realized_pnl=%lld ticks\n",
                name().c_str(),
                fill_count_,
                position(cfg_.instrument_id),
                static_cast<long long>(realized_pnl_));
}

}  // namespace minitrader
