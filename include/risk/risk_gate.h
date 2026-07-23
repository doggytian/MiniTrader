#pragma once

#include <cstdint>
#include <chrono>
#include <unordered_map>
#include "orderbook/order.h"

namespace minitrader {

/// Risk check result.
enum class RiskResult : uint8_t {
    Pass = 0,
    RejectCancelRate,       // Cancel rate too high
    RejectSelfTrade,        // Would self-trade against our own resting order
    RejectPositionLimit,    // Position limit breached
    RejectOrderRate,        // Too many orders per second
};

/// Risk gate configuration.
struct RiskConfig {
    int32_t max_position{1000};          // Max absolute position per instrument
    int32_t max_orders_per_second{100};  // Order rate limit
    double  max_cancel_ratio{0.8};       // Max cancel/total ratio (CSRC rule)
    bool    check_self_trade{true};      // Self-trade prevention
};

/// Inline risk gate — sits in the critical path between strategy and gateway.
///
/// Design principles:
/// - Zero heap allocation in the hot path (unordered_map pre-allocated)
/// - All checks are O(1)
/// - MUST be non-bypassable: strategy cannot send orders without passing through
class RiskGate {
public:
    explicit RiskGate(RiskConfig config) : config_(config) {}

    /// Check if an order passes all risk rules.
    [[nodiscard]] RiskResult check(const Order& order) noexcept;

    /// Update internal state after a fill (position tracking + untrack order).
    void on_fill(const ExecutionReport& report) noexcept;

    /// Reset all counters (e.g., at start of new trading day).
    void reset() noexcept;

    /// Register a newly submitted resting order for self-trade detection.
    /// Called by TradingEngine after a limit order is accepted into the book.
    void track_order(const Order& order) noexcept;

    /// Remove a resting order (cancelled or fully filled).
    void untrack_order(uint64_t order_id) noexcept;

    // ─── Diagnostics ────────────────────────────────────────
    [[nodiscard]] int32_t current_position(uint64_t instrument_id) const noexcept;
    [[nodiscard]] int32_t total_orders_today() const noexcept { return total_orders_; }
    [[nodiscard]] int32_t total_cancels_today() const noexcept { return total_cancels_; }

private:
    [[nodiscard]] bool check_position_limit(const Order& order) const noexcept;
    [[nodiscard]] bool check_cancel_rate() const noexcept;
    [[nodiscard]] bool check_order_rate() noexcept;

    /// Returns false if the incoming order would match against one of our own
    /// resting orders (self-trade prevention).
    /// O(1): looks up by price in the resting-orders map.
    [[nodiscard]] bool check_self_trade(const Order& order) const noexcept;

    RiskConfig config_;

    int32_t position_{0};

    int32_t orders_this_second_{0};
    int32_t total_orders_{0};
    int32_t total_cancels_{0};
    std::chrono::steady_clock::time_point last_rate_reset_;

    /// Active resting orders: order_id → (side, price).
    /// Used for O(1) self-trade detection without touching the order book.
    struct RestingOrder { Side side; int64_t price; };
    std::unordered_map<uint64_t, RestingOrder> active_orders_;
};

}  // namespace minitrader
