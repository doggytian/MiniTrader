#include "engine/trading_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace minitrader {

TradingEngine::TradingEngine(EngineConfig config)
    : config_(config)
    , book_(config.book_config)
    , risk_(config.risk_config)
{
    // 将成交回报回调链接到引擎的 on_fill，再转发给策略
    book_.set_fill_callback([this](const ExecutionReport& r) { on_fill(r); });
}

void TradingEngine::set_strategy(StrategyBase* strategy) {
    if (!strategy) throw std::invalid_argument("策略指针不能为空");
    strategy_ = strategy;
    // 注入引擎指针，使策略的 submit_order/cancel_order/position 生效
    strategy_->engine_ = this;
}

bool TradingEngine::push_tick(const MarketTick& tick) noexcept {
    return tick_queue_.try_push(tick);
}

std::size_t TradingEngine::run_once() {
    std::size_t count = 0;
    while (auto tick = tick_queue_.try_pop()) {
        // ── 延迟计时：出队 → on_tick() 返回 ──────────────────────
        const auto t0 = std::chrono::steady_clock::now();

        if (strategy_) strategy_->on_tick(*tick);

        const auto t1 = std::chrono::steady_clock::now();
        const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

        // 更新延迟统计
        const auto uns = static_cast<uint64_t>(ns);
        latency_sum_ns_ += uns;
        if (uns > latency_max_ns_) latency_max_ns_ = uns;

        // 记入直方图桶：找到第一个桶上界 > uns 的位置
        std::size_t bucket = kHistBuckets - 1;
        for (std::size_t b = 0; b < kHistBounds.size(); ++b) {
            if (uns < kHistBounds[b]) { bucket = b; break; }
        }
        ++latency_hist_[bucket];
        ++ticks_processed_;
        ++count;

        if (config_.enable_latency_log) {
            std::printf("[延迟] tick#%llu  on_tick=%lld ns\n",
                        static_cast<unsigned long long>(ticks_processed_),
                        static_cast<long long>(ns));
        }
    }
    return count;
}

void TradingEngine::run() {
    if (!strategy_) throw std::runtime_error("未绑定策略");
    strategy_->on_start();
    running_.store(true, std::memory_order_release);
    while (running_.load(std::memory_order_acquire)) run_once();
    strategy_->on_stop();
}

void TradingEngine::stop() noexcept {
    running_.store(false, std::memory_order_release);
}

void TradingEngine::submit_order(Order order) {
    const RiskResult result = risk_.check(order);
    if (result != RiskResult::Pass) { ++orders_rejected_; return; }
    ++orders_submitted_;
    // 限价单进入订单簿前先登记，供自成交检测使用
    if (order.type == OrderType::Limit) {
        risk_.track_order(order);
    }
    book_.add_order(std::move(order));
}

void TradingEngine::cancel_order(uint64_t order_id) {
    risk_.untrack_order(order_id);  // 先从自成交跟踪表移除
    book_.cancel_order(order_id);
}

int32_t TradingEngine::position(uint64_t instrument_id) const noexcept {
    return risk_.current_position(instrument_id);
}

uint64_t TradingEngine::latency_percentile_ns(double pct) const noexcept {
    if (ticks_processed_ == 0) return 0;

    // 目标计数（向上取整）
    const uint64_t target = static_cast<uint64_t>(pct / 100.0 * ticks_processed_ + 0.5);
    uint64_t cumulative = 0;

    // 桶下界（前一桶的上界，首桶为 0）
    uint64_t lo = 0;
    for (std::size_t b = 0; b < kHistBuckets; ++b) {
        cumulative += latency_hist_[b];
        const uint64_t hi = (b < kHistBounds.size()) ? kHistBounds[b] : latency_max_ns_;
        if (cumulative >= target) {
            // 在 [lo, hi) 内线性插值
            if (latency_hist_[b] == 0) return lo;
            const double frac = static_cast<double>(target - (cumulative - latency_hist_[b]))
                                / static_cast<double>(latency_hist_[b]);
            return lo + static_cast<uint64_t>(frac * static_cast<double>(hi - lo));
        }
        lo = hi;
    }
    return latency_max_ns_;
}

void TradingEngine::on_fill(const ExecutionReport& report) {
    risk_.on_fill(report);
    // 挂单方完全成交，从自成交跟踪表移除
    if (report.is_maker) {
        risk_.untrack_order(report.order_id);
    }
    ++fills_received_;
    if (strategy_) strategy_->on_fill(report);
}

}  // namespace minitrader
