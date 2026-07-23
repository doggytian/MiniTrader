#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>
#include "orderbook/order.h"
#include "orderbook/price_level.h"

namespace minitrader {

/// 订单簿配置。
struct OrderBookConfig {
    int64_t min_price{0};       // 最低可能价格（tick 数）
    int64_t max_price{10000};   // 最高可能价格（tick 数）
    int64_t tick_size{1};       // 价格最小变动单位
};

/// 基于平坦数组的订单簿，价位访问 O(1)。
///
/// 设计决策：
/// - std::map（红黑树）：O(log N) 但指针追逐严重破坏缓存局部性
/// - 平坦数组：O(1) 访问，对密集价格区间有完美空间局部性
/// - 代价：对稀疏价格区间内存开销较大（大多数品种可接受）
///
/// cancel_order 通过 order_id → (side, price_idx, list::iterator)
/// 哈希表实现 O(1) 撤单。PriceLevel 使用 std::list 保证迭代器插入后不失效。
class OrderBook {
public:
    using FillCallback = std::function<void(const ExecutionReport&)>;

    explicit OrderBook(OrderBookConfig config);

    /// 新增订单（可能触发撮合）。
    void add_order(Order order);

    /// 按 ID 撤单，O(1)。
    /// @return 找到并撤销返回 true，未找到返回 false。
    bool cancel_order(uint64_t order_id);

    /// 最优买价（最高买单价），无买单返回 -1。
    [[nodiscard]] int64_t best_bid() const noexcept;

    /// 最优卖价（最低卖单价），无卖单返回 -1。
    [[nodiscard]] int64_t best_ask() const noexcept;

    /// 买卖价差（tick 数），任意一侧为空返回 -1。
    [[nodiscard]] int64_t spread() const noexcept;

    /// 指定价位的挂单总量。
    [[nodiscard]] int32_t quantity_at(int64_t price, Side side) const noexcept;

    /// 注册成交回报回调。
    void set_fill_callback(FillCallback cb) { fill_callback_ = std::move(cb); }

private:
    /// 挂单位置记录，用于 O(1) cancel。
    struct OrderLocation {
        Side side;
        std::size_t price_idx;
        std::list<Order>::iterator it;
    };

    /// 价格转数组下标。
    [[nodiscard]] std::size_t price_to_index(int64_t price) const noexcept {
        return static_cast<std::size_t>((price - config_.min_price) / config_.tick_size);
    }

    /// 尝试将进场单与挂单撮合。
    void match(Order& incoming);

    /// 通过回调发送成交回报。
    void emit_fill(const ExecutionReport& report);

    /// 某价位清空后，向下扫描更新最优买价。
    void update_best_bid_after_empty(std::size_t exhausted_idx) noexcept;

    /// 某价位清空后，向上扫描更新最优卖价。
    void update_best_ask_after_empty(std::size_t exhausted_idx) noexcept;

    OrderBookConfig config_;
    std::vector<PriceLevel> bids_;  // 按价格索引的买单数组
    std::vector<PriceLevel> asks_;  // 按价格索引的卖单数组

    int64_t best_bid_{-1};
    int64_t best_ask_{-1};

    /// O(1) 撤单查找表：order_id → 在 bids_/asks_ 中的位置。
    std::unordered_map<uint64_t, OrderLocation> order_map_;

    FillCallback fill_callback_;
};

}  // namespace minitrader
