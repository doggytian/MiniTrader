#include "strategy/strategy_base.h"
#include "engine/trading_engine.h"

namespace minitrader {

void StrategyBase::submit_order(Order order) {
    if (engine_) engine_->submit_order(std::move(order));
}

void StrategyBase::cancel_order(uint64_t order_id) {
    if (engine_) engine_->cancel_order(order_id);
}

int32_t StrategyBase::position(uint64_t instrument_id) const {
    if (engine_) return engine_->position(instrument_id);
    return 0;
}

}  // namespace minitrader
