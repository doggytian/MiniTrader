#pragma once

#include <cstdint>
#include <chrono>
#include "orderbook/order.h"

namespace minitrader {

/// Risk check result.
enum class RiskResult : uint8_t {
    Pass = 0,
    RejectCancelRate,       // Cancel rate too high
    RejectSelfTrade,        // Would self-trade
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
/// - Zero heap allocation in the hot path
/// - All checks are O(1)
/// - Compile-time configurable via template policies (future)
/// - MUST be non-bypassable: strategy cannot send orders without passing through
///
/// This is the "seatbelt" of the trading system. Even a bug in strategy code
/// cannot cause uncontrolled risk if this gate is correct.
class RiskGate {
public:
    explicit RiskGate(RiskConfig config) : config_(config) {}

    /// Check if an order passes all risk rules.
    /// @return RiskResult::Pass if OK, or the specific rejection reason.
    [[nodiscard]] RiskResult check(const Order& order) noexcept;

    /// Update internal state after a fill (position tracking).
    void on_fill(const ExecutionReport& report) noexcept;

    /// Reset all counters (e.g., at start of new trading day).
    void reset() noexcept;

    // ─── Diagnostics ────────────────────────────────────────
    [[nodiscard]] int32_t current_position(uint64_t instrument_id) const noexcept;
    [[nodiscard]] int32_t total_orders_today() const noexcept { return total_orders_; }
    [[nodiscard]] int32_t total_cancels_today() const noexcept { return total_cancels_; }

private:
    [[nodiscard]] bool check_position_limit(const Order& order) const noexcept;
    [[nodiscard]] bool check_cancel_rate() const noexcept;
    [[nodiscard]] bool check_order_rate() noexcept;
    [[nodiscard]] bool check_self_trade(const Order& order) const noexcept;

    RiskConfig config_;

    // Position tracking (simple version: single instrument)
    int32_t position_{0};

    // Order rate tracking
    int32_t orders_this_second_{0};
    int32_t total_orders_{0};
    int32_t total_cancels_{0};
    std::chrono::steady_clock::time_point last_rate_reset_;
};

}  // namespace minitrader
