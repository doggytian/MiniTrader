#pragma once

#include <cstdint>
#include <string>
#include "strategy/strategy_base.h"

namespace minitrader {

class TradingEngine;  // forward declaration for cancel_resting_quotes

struct SpreadStrategyConfig {
    uint64_t instrument_id{1};
    int64_t  half_spread{1};    // ticks each side from mid
    int32_t  order_size{10};    // lots per side
    bool     verbose{true};     // print quotes/fills to stdout
};

/// Simple spread-making strategy.
///
/// Logic:
///   On each tick, if we have no resting orders:
///     - Place a buy  limit at (mid - half_spread)
///     - Place a sell limit at (mid + half_spread)
///   On fill:
///     - Cancel the unfilled side (avoid one-sided position buildup)
///     - Let the next tick re-quote both sides
///
/// This is the minimal skeleton of a market-making strategy.
/// It is NOT a production system — position limits, inventory skew,
/// adverse selection filters, and fee math are deliberately omitted.
class SpreadStrategy : public StrategyBase {
public:
    explicit SpreadStrategy(SpreadStrategyConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] std::string name() const override { return "SpreadStrategy"; }

    void on_start() override;
    void on_tick(const MarketTick& tick) override;
    void on_fill(const ExecutionReport& report) override;
    void on_stop() override;

    /// Cancel resting quotes and reset internal IDs (for benchmarking).
    void cancel_resting_quotes(TradingEngine& engine);

private:
    uint64_t next_order_id() noexcept { return ++next_id_; }

    SpreadStrategyConfig cfg_;
    uint64_t next_id_{0};

    // IDs of currently resting quotes (0 = not live)
    uint64_t bid_order_id_{0};
    uint64_t ask_order_id_{0};

    // P&L tracking (tick × lots, not real currency)
    int64_t  realized_pnl_{0};
    int32_t  fill_count_{0};
};

}  // namespace minitrader
