#pragma once

#include <string>
#include "orderbook/order.h"

namespace minitrader {

// Forward declaration to avoid circular include
class TradingEngine;

/// Market data tick event.
struct MarketTick {
    uint64_t instrument_id;
    int64_t  bid_price;
    int64_t  ask_price;
    int32_t  bid_size;
    int32_t  ask_size;
    int64_t  last_price;
    int32_t  last_size;
    uint64_t exchange_timestamp_ns;
    uint64_t local_timestamp_ns;
};

/// Abstract strategy base class (Template Method pattern).
///
/// Derived strategies implement on_tick() and on_fill().
/// The framework handles order routing and risk checks transparently —
/// strategy code calls submit_order() / cancel_order() / position() and
/// never touches OrderBook or RiskGate directly.
class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    /// Called on each market data update.
    virtual void on_tick(const MarketTick& tick) = 0;

    /// Called when an order is filled (either maker or taker side).
    virtual void on_fill(const ExecutionReport& report) = 0;

    /// Called once before the first tick (initialize state, pre-load data).
    virtual void on_start() {}

    /// Called once after the last tick (cleanup, report PnL).
    virtual void on_stop() {}

    /// Strategy name used for logging.
    [[nodiscard]] virtual std::string name() const = 0;

protected:
    /// Submit a new order.  Routes through RiskGate → OrderBook.
    /// Rejected orders are silently dropped (check engine diagnostics).
    void submit_order(Order order);

    /// Cancel a resting order by id.
    void cancel_order(uint64_t order_id);

    /// Net position for an instrument (positive = long, negative = short).
    [[nodiscard]] int32_t position(uint64_t instrument_id) const;

private:
    friend class TradingEngine;
    TradingEngine* engine_{nullptr};  // injected by TradingEngine::set_strategy()
};

}  // namespace minitrader
