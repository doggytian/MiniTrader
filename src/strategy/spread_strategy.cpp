#include "strategy/spread_strategy.h"
#include "engine/trading_engine.h"

#include <algorithm>
#include <cstdio>
#include <cmath>

namespace minitrader {

void SpreadStrategy::on_start() {
    if (!cfg_.verbose) return;
    std::printf("[%s] 启动  基础半价差=%lld tick  每侧数量=%d  最大持仓=%d\n",
                name().c_str(),
                static_cast<long long>(cfg_.half_spread),
                cfg_.order_size,
                cfg_.max_position);
}

void SpreadStrategy::on_tick(const MarketTick& tick) {
    if (tick.instrument_id != cfg_.instrument_id) return;
    if (tick.bid_price <= 0 || tick.ask_price <= 0) return;

    // ── 更新市场价差 EMA（波动率代理）─────────────────────────────────────
    const double market_spread = static_cast<double>(tick.ask_price - tick.bid_price);
    if (!ema_initialized_) {
        ema_spread_     = market_spread;
        ema_initialized_ = true;
    } else {
        ema_spread_ = cfg_.ema_alpha * market_spread
                    + (1.0 - cfg_.ema_alpha) * ema_spread_;
    }

    // 已有挂单 → 等待成交或被撤，不重复报价
    if (bid_order_id_ != 0 || ask_order_id_ != 0) return;

    // ── 持仓超限 → 暂停双边报价，等待成交降仓 ─────────────────────────
    const int32_t pos = position(cfg_.instrument_id);
    if (std::abs(pos) >= cfg_.max_position) {
        if (cfg_.verbose) {
            std::printf("[%s] 持仓超限(%d) 暂停报价\n",
                        name().c_str(), pos);
        }
        return;
    }

    // ── Inventory Skew：根据净持仓偏移中间价 ───────────────────────────
    // 净多头 → 中间价下移 → 卖价更低，更容易成交，驱动平仓
    // 净空头 → 中间价上移 → 买价更高，更容易买入，驱动平仓
    const int64_t raw_mid = (tick.bid_price + tick.ask_price) / 2;
    const int64_t skew_offset = static_cast<int64_t>(
        std::round(-pos * cfg_.skew_per_lot));   // 多头为负，空头为正
    const int64_t mid = raw_mid + skew_offset;

    // ── 波动率自适应：市场价差过宽时加宽我方 half_spread ─────────────────
    int64_t half = cfg_.half_spread;
    if (ema_spread_ > 0.0 &&
        market_spread > ema_spread_ * cfg_.spread_scale_threshold) {
        half = static_cast<int64_t>(
            std::ceil(half * cfg_.spread_scale_factor));
    }

    const int64_t buy_px  = mid - half;
    const int64_t sell_px = mid + half;

    if (buy_px <= 0 || sell_px <= buy_px) return;

    // ── 双边报价 ──────────────────────────────────────────────────────
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
        const char* skew_tag = (skew_offset < 0) ? "↓skew" :
                               (skew_offset > 0) ? "↑skew" : "flat";
        const char* vol_tag  = (half > cfg_.half_spread) ? " [宽价差]" : "";
        std::printf("[%s] 报价  买=%lld  卖=%lld  持仓=%d  %s%s\n",
                    name().c_str(),
                    static_cast<long long>(buy_px),
                    static_cast<long long>(sell_px),
                    pos, skew_tag, vol_tag);
    }
}

void SpreadStrategy::on_fill(const ExecutionReport& report) {
    ++fill_count_;

    const bool filled_bid = (report.order_id == bid_order_id_);
    const bool filled_ask = (report.order_id == ask_order_id_);

    if (filled_bid) {
        if (ask_order_id_ != 0) {
            cancel_order(ask_order_id_);
            ask_order_id_ = 0;
        }
        bid_order_id_  = 0;
        realized_pnl_ -= report.price * report.filled_quantity;
    } else if (filled_ask) {
        if (bid_order_id_ != 0) {
            cancel_order(bid_order_id_);
            bid_order_id_ = 0;
        }
        ask_order_id_  = 0;
        realized_pnl_ += report.price * report.filled_quantity;
    }

    if (cfg_.verbose) {
        std::printf("[%s] 成交  方向=%s  价格=%lld  数量=%d  持仓=%d  累计=%d\n",
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
