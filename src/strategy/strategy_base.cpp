#include "strategy/strategy_base.h"

namespace minitrader {

// TODO: Implement order routing (backtest vs. live mode dispatch)
void StrategyBase::submit_order(Order order) {
    (void)order;
}

void StrategyBase::cancel_order(uint64_t order_id) {
    (void)order_id;
}

int32_t StrategyBase::position(uint64_t instrument_id) const {
    (void)instrument_id;
    return 0;
}

}  // namespace minitrader
