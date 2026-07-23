#pragma once

#include <cstdint>
#include <chrono>

namespace minitrader {

/// 买卖方向。
enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1,
};

/// 订单类型。
enum class OrderType : uint8_t {
    Limit  = 0,  // 限价单
    Market = 1,  // 市价单
    Cancel = 2,  // 撤单
};

/// 紧凑订单结构（适配一个 cache line）。
/// 所有价格以整数 tick 表示，避免浮点运算。
struct Order {
    uint64_t  order_id;       // 唯一订单 ID
    uint64_t  instrument_id;  // 品种 / 合约 ID
    int64_t   price;          // 价格（tick 数，市价单填 0）
    int32_t   quantity;       // 剩余数量
    Side      side;           // 买 / 卖
    OrderType type;           // 订单类型
    uint16_t  reserved{0};    // 对齐填充
    uint64_t  timestamp_ns;   // 纳秒时间戳（steady_clock）

    /// 获取当前纳秒时间戳。
    static uint64_t now_ns() noexcept {
        auto tp = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                tp.time_since_epoch()).count());
    }
};

static_assert(sizeof(Order) <= 64, "Order 必须能放入一个 cache line");

/// 成交回报，撮合后向买卖双方各发送一条。
struct ExecutionReport {
    uint64_t order_id;          // 本方订单 ID
    uint64_t matched_order_id;  // 对手方订单 ID
    int64_t  price;             // 成交价格
    int32_t  filled_quantity;   // 成交数量
    Side     side;              // 本方方向
    bool     is_maker;          // true = 本方为挂单方（maker）
    uint64_t timestamp_ns;      // 成交时间戳
};

}  // namespace minitrader
