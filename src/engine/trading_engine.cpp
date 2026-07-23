#include "engine/trading_engine.h"

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
        latency_sum_ns_ += static_cast<uint64_t>(ns);
        if (static_cast<uint64_t>(ns) > latency_max_ns_) latency_max_ns_ = static_cast<uint64_t>(ns);
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
