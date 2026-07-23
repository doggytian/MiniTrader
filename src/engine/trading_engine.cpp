#include "engine/trading_engine.h"

#include <stdexcept>

namespace minitrader {

TradingEngine::TradingEngine(EngineConfig config)
    : config_(config)
    , book_(config.book_config)
    , risk_(config.risk_config)
{
    // Wire fill callback: order book fires fills → engine → strategy
    book_.set_fill_callback([this](const ExecutionReport& r) { on_fill(r); });
}

void TradingEngine::set_strategy(StrategyBase* strategy) {
    if (!strategy) throw std::invalid_argument("strategy must not be null");
    strategy_ = strategy;
    // Inject engine pointer so StrategyBase::submit_order/cancel/position work
    strategy_->engine_ = this;
}

bool TradingEngine::push_tick(const MarketTick& tick) noexcept {
    return tick_queue_.try_push(tick);
}

std::size_t TradingEngine::run_once() {
    std::size_t count = 0;
    while (auto tick = tick_queue_.try_pop()) {
        if (strategy_) {
            strategy_->on_tick(*tick);
        }
        ++ticks_processed_;
        ++count;
    }
    return count;
}

void TradingEngine::run() {
    if (!strategy_) throw std::runtime_error("No strategy attached");
    strategy_->on_start();
    running_.store(true, std::memory_order_release);

    while (running_.load(std::memory_order_acquire)) {
        run_once();
    }

    strategy_->on_stop();
}

void TradingEngine::stop() noexcept {
    running_.store(false, std::memory_order_release);
}

void TradingEngine::submit_order(Order order) {
    const RiskResult result = risk_.check(order);
    if (result != RiskResult::Pass) {
        ++orders_rejected_;
        return;
    }
    ++orders_submitted_;
    book_.add_order(std::move(order));
}

void TradingEngine::cancel_order(uint64_t order_id) {
    book_.cancel_order(order_id);
}

int32_t TradingEngine::position(uint64_t instrument_id) const noexcept {
    return risk_.current_position(instrument_id);
}

void TradingEngine::on_fill(const ExecutionReport& report) {
    risk_.on_fill(report);
    ++fills_received_;
    if (strategy_) {
        strategy_->on_fill(report);
    }
}

}  // namespace minitrader
