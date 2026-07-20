#pragma once

#include <string>
#include "orderbook/order.h"

namespace minitrader {

/// Market data tick event.
struct MarketTick {
    uint64_t instrument_id;
    int64_t  bid_price;
    int64_t  ask_price;
    int32_t  bid_size;
    int32_t  ask_size;
    int64_t  last_price;
    int32_t  last_size;
    uint64_t exchange_timestamp_ns;  // Exchange timestamp
    uint64_t local_timestamp_ns;     // Local receive timestamp
};

/// Abstract strategy base class (Template Method pattern).
///
/// Derived strategies implement on_tick() and on_fill().
/// The framework handles data routing, backtesting vs. live mode switching,
/// and order submission — strategy code is identical in both modes.
class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    /// Called on each market data update.
    virtual void on_tick(const MarketTick& tick) = 0;

    /// Called when an order is filled.
    virtual void on_fill(const ExecutionReport& report) = 0;

    /// Called once at strategy start (initialize state).
    virtual void on_start() {}

    /// Called once at strategy stop (cleanup).
    virtual void on_stop() {}

    /// Get strategy name (for logging/identification).
    [[nodiscard]] virtual std::string name() const = 0;

protected:
    /// Submit a new order (routed through risk gate).
    /// In backtest mode, this goes to the simulated matching engine.
    /// In live mode, this goes to the real exchange gateway.
    void submit_order(Order order);

    /// Cancel an existing order.
    void cancel_order(uint64_t order_id);

    /// Get current position for an instrument.
    [[nodiscard]] int32_t position(uint64_t instrument_id) const;

private:
    // Framework internals (order routing, mode switching)
    // Implemented in strategy_base.cpp
    friend class TradingEngine;
};

}  // namespace minitrader
