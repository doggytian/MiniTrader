#pragma once

#include <cstdint>
#include <deque>
#include "orderbook/order.h"

namespace minitrader {

/// A single price level in the order book.
/// Orders at the same price are stored in FIFO (time priority).
struct PriceLevel {
    int64_t price{0};
    int32_t total_quantity{0};
    std::deque<Order> orders;  // FIFO queue of resting orders

    bool empty() const noexcept { return orders.empty(); }

    void add_order(Order order) {
        total_quantity += order.quantity;
        orders.push_back(std::move(order));
    }

    /// Remove the front order (after full fill).
    void remove_front() {
        if (!orders.empty()) {
            total_quantity -= orders.front().quantity;
            orders.pop_front();
        }
    }

    /// Reduce front order quantity (partial fill).
    void reduce_front(int32_t filled) {
        if (!orders.empty()) {
            orders.front().quantity -= filled;
            total_quantity -= filled;
        }
    }
};

}  // namespace minitrader
