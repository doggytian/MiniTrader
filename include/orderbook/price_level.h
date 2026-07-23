#pragma once

#include <cstdint>
#include <list>
#include "orderbook/order.h"

namespace minitrader {

/// 订单簿中单个价位的挂单队列。
/// 同价位按时间优先（FIFO）排列。
///
/// 使用 std::list 而非 std::deque：list 的迭代器在任意插入/删除后永远有效，
/// 这是 OrderBook::cancel_order O(1) 实现的前提——
/// 通过缓存 list::iterator 可以直接 erase，无需线性扫描。
struct PriceLevel {
    int64_t         price{0};          // 该价位的价格（tick）
    int32_t         total_quantity{0}; // 该价位挂单总量
    std::list<Order> orders;           // FIFO 挂单队列

    bool empty() const noexcept { return orders.empty(); }

    /// 新增挂单（追加到队尾）。
    void add_order(Order order) {
        total_quantity += order.quantity;
        orders.push_back(std::move(order));
    }

    /// 移除队首订单（完全成交后调用）。
    void remove_front() {
        if (!orders.empty()) {
            total_quantity -= orders.front().quantity;
            orders.pop_front();
        }
    }

    /// 减少队首订单数量（部分成交后调用）。
    void reduce_front(int32_t filled) {
        if (!orders.empty()) {
            orders.front().quantity -= filled;
            total_quantity -= filled;
        }
    }

    /// 按迭代器删除任意订单（O(1)，供 cancel_order 使用）。
    /// 调用方负责传入有效迭代器；函数内部同步更新 total_quantity。
    void erase(std::list<Order>::iterator it) {
        total_quantity -= it->quantity;
        orders.erase(it);
    }
};

}  // namespace minitrader
