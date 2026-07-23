#include "strategy/spread_strategy.h"
#include "engine/trading_engine.h"

#include <cstdio>

namespace minitrader {

void SpreadStrategy::on_start() {
    if (!cfg_.verbose) return;
    std::printf("[%s] 启动  半价差=%lld tick  每侧数量=%d\n",
                name().c_str(),
                static_cast<long long>(cfg_.half_spread),
                cfg_.order_size);
}

void SpreadStrategy::on_tick(const MarketTick& tick) {
    if (tick.instrument_id != cfg_.instrument_id) return;
    if (tick.bid_price <= 0 || tick.ask_price <= 0) return;

    // 双侧均有挂单时不重新报价
    if (bid_order_id_ != 0 || ask_order_id_ != 0) return;

    const int64_t mid     = (tick.bid_price + tick.ask_price) / 2;
    const int64_t buy_px  = mid - cfg_.half_spread;
    const int64_t sell_px = mid + cfg_.half_spread;

    if (buy_px <= 0 || sell_px <= buy_px) return;

    // 挂买单
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

    // 挂卖单
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

    if (cfg_.verbose) {
        std::printf("[%s] 报价  买=%lld  卖=%lld  持仓=%d\n",
                    name().c_str(),
                    static_cast<long long>(buy_px),
                    static_cast<long long>(sell_px),
                    position(cfg_.instrument_id));
    }
}

void SpreadStrategy::on_fill(const ExecutionReport& report) {
    ++fill_count_;

    const bool filled_bid = (report.order_id == bid_order_id_);
    const bool filled_ask = (report.order_id == ask_order_id_);

    if (filled_bid) {
        // 买单成交：撤销对侧卖单，防止单侧持仓积累
        if (ask_order_id_ != 0) {
            cancel_order(ask_order_id_);
            ask_order_id_ = 0;
        }
        bid_order_id_  = 0;
        realized_pnl_ -= report.price * report.filled_quantity;
    } else if (filled_ask) {
        // 卖单成交：撤销对侧买单
        if (bid_order_id_ != 0) {
            cancel_order(bid_order_id_);
            bid_order_id_ = 0;
        }
        ask_order_id_  = 0;
        realized_pnl_ += report.price * report.filled_quantity;
    }

    if (cfg_.verbose) {
        std::printf("[%s] 成交  方向=%s  价格=%lld  数量=%d  持仓=%d  累计成交=%d\n",
                    name().c_str(),
                    report.side == Side::Buy ? "买" : "卖",
                    static_cast<long long>(report.price),
                    report.filled_quantity,
                    position(cfg_.instrument_id),
                    fill_count_);
    }
}

void SpreadStrategy::on_stop() {
    if (!cfg_.verbose) return;
    std::printf("[%s] 停止  成交次数=%d  持仓=%d  已实现盈亏=%lld tick\n",
                name().c_str(),
                fill_count_,
                position(cfg_.instrument_id),
                static_cast<long long>(realized_pnl_));
}

void SpreadStrategy::cancel_resting_quotes(TradingEngine& engine) {
    if (bid_order_id_ != 0) {
        engine.cancel_order(bid_order_id_);
        bid_order_id_ = 0;
    }
    if (ask_order_id_ != 0) {
        engine.cancel_order(ask_order_id_);
        ask_order_id_ = 0;
    }
}

}  // namespace minitrader
