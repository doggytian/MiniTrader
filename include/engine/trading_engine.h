#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/spsc_queue.h"
#include "orderbook/order_book.h"
#include "risk/risk_gate.h"
#include "strategy/strategy_base.h"

namespace minitrader {

/// Configuration for TradingEngine.
struct EngineConfig {
    OrderBookConfig book_config{.min_price = 1, .max_price = 100000, .tick_size = 1};
    RiskConfig      risk_config{};
    std::size_t     tick_queue_capacity{4096};  // informational only; queue size is fixed
    bool            enable_latency_log{false};  // print per-tick on_tick() latency
};

/// TradingEngine — the central coordinator.
///
/// Wiring:
///   MarketTick --> [SPSC queue] --> TradingEngine::run_once()
///                                       --> strategy.on_tick()
///                                           --> strategy.submit_order()
///                                               --> RiskGate.check()
///                                                   --> OrderBook.add_order()
///                                                       --> fill_callback
///                                                           --> strategy.on_fill()
///
/// Thread model (single-threaded demo):
///   - push_tick()  called by producer (market data thread or test harness)
///   - run_once() / run() called by consumer (strategy thread)
///   Both sides communicate via SPSCQueue — zero mutex in the hot path.
class TradingEngine {
public:
    static constexpr std::size_t kQueueCap = 4096;  // must be power of 2

    explicit TradingEngine(EngineConfig config = {});

    // Non-copyable
    TradingEngine(const TradingEngine&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    /// Attach a strategy. Must be called before run()/run_once().
    /// The engine injects itself into the strategy so that submit_order()
    /// and cancel_order() route through the risk gate and order book.
    void set_strategy(StrategyBase* strategy);

    /// Enqueue a market tick (producer side, lock-free).
    /// @return false if queue is full (tick dropped).
    [[nodiscard]] bool push_tick(const MarketTick& tick) noexcept;

    /// Drain all pending ticks and process them synchronously (consumer side).
    /// Returns the number of ticks processed.
    std::size_t run_once();

    /// Run until stop() is called (blocking loop, consumer side).
    void run();

    /// Signal the run() loop to exit.
    void stop() noexcept;

    // ─── Called by StrategyBase (friend) ────────────────────────────────────
    /// Submit an order through risk gate → order book.
    void submit_order(Order order);

    /// Cancel a resting order.
    void cancel_order(uint64_t order_id);

    /// Current net position for an instrument.
    [[nodiscard]] int32_t position(uint64_t instrument_id) const noexcept;

    // ─── Diagnostics ────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t ticks_processed() const noexcept { return ticks_processed_; }
    [[nodiscard]] uint64_t orders_submitted() const noexcept { return orders_submitted_; }
    [[nodiscard]] uint64_t orders_rejected() const noexcept { return orders_rejected_; }
    [[nodiscard]] uint64_t fills_received() const noexcept { return fills_received_; }

    /// Average on_tick() latency in nanoseconds (0 if no ticks processed).
    [[nodiscard]] uint64_t latency_avg_ns() const noexcept {
        return ticks_processed_ ? latency_sum_ns_ / ticks_processed_ : 0;
    }
    /// Peak on_tick() latency in nanoseconds.
    [[nodiscard]] uint64_t latency_max_ns() const noexcept { return latency_max_ns_; }

    [[nodiscard]] const OrderBook& order_book() const noexcept { return book_; }
    [[nodiscard]] const RiskGate&  risk_gate()  const noexcept { return risk_; }

private:
    void on_fill(const ExecutionReport& report);

    EngineConfig config_;
    OrderBook    book_;
    RiskGate     risk_;

    SPSCQueue<MarketTick, kQueueCap> tick_queue_;

    StrategyBase* strategy_{nullptr};

    std::atomic<bool> running_{false};

    // Counters
    uint64_t ticks_processed_{0};
    uint64_t orders_submitted_{0};
    uint64_t orders_rejected_{0};
    uint64_t fills_received_{0};

    // Latency stats (on_tick wall-clock, nanoseconds)
    uint64_t latency_sum_ns_{0};
    uint64_t latency_max_ns_{0};
};

}  // namespace minitrader
