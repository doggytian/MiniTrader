# MiniTrader 架构详解

> 本文档用于理解项目各模块的设计决策，面试讲解参考。

---

## 一、整体定位

MiniTrader 是一个**低延迟交易引擎的核心层实现**，用 C++20 写，目标是演示"如何在 market data 到 order 这条路径上把延迟压到极限"。它**不是**一个完整的交易平台，而是一个专注于热路径（hot path）的引擎骨架。

整个数据流只有一条：

```
[网络] → MarketTick → [SPSC Queue] → [Strategy] → Order → [Risk Gate] → Order Gateway
```

每一步都有明确的设计决策，这是讲项目的核心主线。

---

## 二、五个模块逐一讲透

### 模块 1：`SPSCQueue`（`include/core/spsc_queue.h`）

**它是什么**：无锁的单生产者单消费者环形队列，连接"行情接收线程"和"策略线程"。

**为什么要它**：如果用 `mutex`，每次行情推送都要加锁解锁，操作系统可能触发线程调度，延迟从 50ns 跳到 10μs+。SPSC 天然是一生产一消费，不存在竞争，可以完全去掉锁。

**核心设计决策**：

**① Power-of-2 容量 + 位掩码取模**

`(index + 1) & kMask` 代替 `% Cap`，避免除法指令，编译器直接优化成一条 AND 指令。这是编译期约束（C++20 `requires` 表达式）：
```cpp
template <typename T, std::size_t Cap>
    requires (Cap > 0 && (Cap & (Cap - 1)) == 0)  // 必须是 2 的幂
```

**② `memory_order_acquire/release` 而非 `seq_cst`**

- `seq_cst`（默认）会插入全内存屏障（`MFENCE`），在 x86 上每次约 +10ns 额外开销
- SPSC 只需要两个方向的保证：
  - producer 写完数据后，`release` 写 `tail`，consumer `acquire` 读 `tail`，保证看到新数据
  - consumer 消费完后，`release` 写 `head`，producer `acquire` 读 `head`，保证看到空槽
- 不需要全局顺序，用 acquire/release 就够了，比 `seq_cst` 少一条 fence

**③ Cache-line padding（false sharing 防护）**

```cpp
alignas(64) atomic<size_t> head_;
char pad1_[64 - sizeof(atomic<size_t>)];   // 填满一个 cache line

alignas(64) atomic<size_t> tail_;
char pad2_[...];
```

`head_` 和 `tail_` 放在不同 cache line。原因：现代 CPU 以 64 字节 cache line 为单位传输数据。如果两者在同一行，producer 写 `tail` 会导致 consumer 所在 CPU 的 cache line 失效（MESI 协议 invalidate），即使 consumer 根本没读 `tail`——这就是 false sharing，代价约 60ns/次。分开后两个线程完全操作不同的 cache line，零竞争。

**④ Placement new + 手动析构**

元素用 raw bytes 数组存储，push 时 `new(&storage_[tail]) T(...)` 原地构造，pop 时 `elem->~T()` 手动析构，完全避免动态内存分配（`malloc/free` 在热路径上代价不可控）。

**当前 TODO**：
- `try_pop` 返回 `std::optional<T>` 涉及一次移动构造，对于大对象有开销，更好的 API 是传出参（引用）
- `aligned_storage_t` 在 C++23 被 deprecated，应改为 `alignas(T) std::byte storage_[sizeof(T) * Cap]`

---

### 模块 2：`OrderBook`（`include/orderbook/` + `src/orderbook/order_book.cpp`）

**它是什么**：价格-时间优先的限价订单簿，支持 add/cancel/match，同时维护最优买卖价。

**为什么不用 `std::map`**

`std::map` 是红黑树，每个节点是堆上独立分配的，查找一个价格要走 O(log N) 次指针跳转。每次指针跳转都可能 cache miss，代价约 100ns。对于价格范围有界（如期货涨跌停）且 tick 密集的订单簿，用 flat array 按价格直接索引：

```cpp
index = (price - min_price) / tick_size
```

O(1) 访问，所有 level 在连续内存里，CPU prefetcher 友好。代价是内存：`price_range × sizeof(PriceLevel)`，对于单品种完全可接受。

**数据结构（两层）**：

- 第一层：`vector<PriceLevel> bids_` / `asks_`，下标就是价格 index，直接寻址
- 第二层：每个 `PriceLevel` 内有一个 `deque<Order>`，按时间 FIFO——保证时间优先

**撮合逻辑（`match()`）**：

- 买单进来：找 `best_ask`，如果 `buy_price >= best_ask`，就和 ask 队列头部成交
- 成交后 level 为空时线性扫描找下一个非空价格，更新 `best_ask`
- 每次成交调 `emit_fill()`，向两边各发一个 `ExecutionReport`（`is_maker` 区分挂单方/吃单方）
- 有剩余量且是 Limit 单：挂入 book，更新 `best_bid/ask`

**`Order` 结构设计细节**：

```cpp
static_assert(sizeof(Order) <= 64, "Order must fit in a cache line");
```

整个 Order 设计成 ≤64 字节，一次 cache line fetch 就能加载完整。价格用 integer ticks 而非 double，避免浮点比较的精度问题（两个 `double` 相等判断是 UB 的来源之一）。

**`ExecutionReport` 的 `is_maker` 字段**：区分挂单方（maker）和吃单方（taker）。实盘中交易所对 maker 收更低手续费（甚至返佣），所以这个字段对 PnL 计算有意义。

**当前问题：cancel_order 是空的**

```cpp
bool OrderBook::cancel_order(uint64_t order_id) {
    // TODO: O(1) cancel via order_id -> location map
    (void)order_id;
    return false;
}
```

正确做法：维护 `unordered_map<uint64_t order_id, {side, price_index}>` 的 location map，cancel 时 O(1) 定位到 level，再从 `deque` 中删除（O(N) in level，实际 level 内单数不多）。这是面试前必须修掉的。

---

### 模块 3：`RiskGate`（`include/risk/risk_gate.h`）

**它是什么**：内联风控关卡，strategy 发出的每个 order 必须经过它，失败直接拒绝，不走后续流程。

**四项检查**：

| 检查 | 实现 | 对应需求 |
|---|---|---|
| `check_order_rate()` | 滑动窗口计数（1 秒重置），超过 `max_orders_per_second` 拒绝 | 防止策略 bug 导致报单洪流 |
| `check_position_limit()` | 预计算 `projected = current ± new_qty`，超 `max_position` 拒绝 | 防止持仓失控 |
| `check_cancel_rate()` | `total_cancels / total_orders > 0.8` 拒绝 | 对应证监会报撤比规则（80%） |
| `check_self_trade()` | 目前是 stub | 防止自成交（拉抬操纵） |

**设计亮点**：

- 所有检查都是 O(1)，热路径无堆分配
- `noexcept` 标注：告诉编译器不会抛异常，避免生成异常表查询代码
- `[[nodiscard]]`：强制调用方处理返回值，不能默默忽略风控结果

**`on_fill()` 反馈闭环**：

```cpp
void RiskGate::on_fill(const ExecutionReport& report) noexcept {
    if (report.side == Side::Buy)  position_ += report.filled_quantity;
    else                           position_ -= report.filled_quantity;
}
```

成交后主动回调，更新持仓，保证风控状态和实际持仓同步。如果不做这个，position check 会基于错误的持仓数判断。

**当前 limitation**：`position_` 是单个 int，只支持单品种。多品种需要 `unordered_map<uint64_t instrument_id, int32_t>`。

---

### 模块 4：`MarketReceiver`（`include/network/market_receiver.h`）

**它是什么**：行情接收器，Linux 上用 epoll 监听 UDP 组播，macOS 上是 stub（仅开发用）。

**为什么 epoll 边缘触发（ET）而非水平触发（LT）**：

- **LT（Level-Triggered）**：只要 socket buffer 里有数据，`epoll_wait` 每次都返回。如果没读完，下次还会通知——安全但每次都要走一次 syscall
- **ET（Edge-Triggered）**：只在 buffer **从空变为非空**时通知一次。必须循环 `recv` 直到 `EAGAIN`（buffer 读空）才算完成
- 在高频行情下（每秒数万 tick），LT 的重复唤醒每次约 1μs syscall 开销会积累成明显延迟；ET 把多次通知压缩成一次，减少内核态切换次数

**关键配置细节**：

- `SOCK_NONBLOCK`：socket 本身非阻塞，确保 `recv` 遇到空 buffer 立即返回 `EAGAIN` 而不是阻塞
- `SO_RCVBUF = 4MB`：大内核接收缓冲区，应对行情突发（如开盘瞬间），防止内核因 buffer 满而丢包
- `sched_setaffinity`：把接收线程绑定到指定 CPU core，减少线程迁移导致的 cache miss

**当前状态**：epoll 事件循环骨架是完整的，`recv` 到 raw bytes 后有 `TODO: decode protocol`——协议解码没实现（个人项目无法接入真实交易所）。接口已预留好，填 decoder 就能接真实行情。

---

### 模块 5：`StrategyBase`（`include/strategy/strategy_base.h`）

**它是什么**：策略的抽象基类，用 **Template Method 模式**。派生类只需实现业务逻辑，框架负责数据路由。

**接口设计**：

```cpp
// 派生类必须实现
virtual void on_tick(const MarketTick& tick) = 0;     // 行情驱动
virtual void on_fill(const ExecutionReport& report) = 0; // 成交回调
virtual std::string name() const = 0;

// 可选覆盖
virtual void on_start() {}  // 策略启动
virtual void on_stop()  {}  // 策略停止

// 框架提供（protected，策略可用）
void submit_order(Order order);           // 发单（经过 RiskGate）
void cancel_order(uint64_t order_id);     // 撤单
int32_t position(uint64_t instrument_id) const; // 查持仓
```

**`friend class TradingEngine`**：`submit_order` 等 protected 方法的实际路由逻辑由 `TradingEngine` 注入，策略代码不感知"现在是回测还是实盘"——这就是 Template Method 的核心价值，策略代码在两种模式下完全一致。

**当前状态**：`submit_order` / `cancel_order` / `position` 实现都是空的（`(void)param`），`TradingEngine` 类不存在。接口设计是对的，实现待补。

---

## 三、整体的缺口

| 状态 | 内容 |
|---|---|
| ✅ 完成 | 5 个模块的接口和数据结构，核心算法（SPSC、撮合、风控检查） |
| ⚠️ 残缺 | `cancel_order`（空实现）、`check_self_trade`（stub）、`MarketReceiver` 解码 |
| ❌ 不存在 | `TradingEngine`（模块串联）、`StrategyBase` 路由实现、回测引擎 |

项目现在是**"设计正确的组件库"**，还不是**"能端到端跑的系统"**。

**面试中怎么讲这个缺口**：

> "这个项目我的重点是把每个核心模块的低延迟设计做扎实、做可解释，而不是堆功能。TradingEngine 和协议 decode 我没有做，因为对于个人项目来说接真实交易所需要券商账号，而 Engine 的串联逻辑相对简单。我能清楚描述它应该怎么做，以及我在生产环境里如何处理类似的数据管道问题。"

---

## 四、面试讲法结构（参考）

```
1. 一句话定位（30 秒）
   "一个 C++20 的低延迟交易引擎骨架，聚焦 market data → order 热路径"

2. 画架构图（2 分钟）
   Network → SPSC → Strategy → Risk Gate → Gateway
   每个箭头说清楚为什么是这个技术选型

3. 深挖一个模块（10 分钟，通常面试官决定挖哪个）
   SPSC：memory_order、false sharing、placement new
   OrderBook：flat array vs map、price_to_index、FIFO 撮合
   Risk：四项检查、noexcept、与 on_fill 联动

4. 主动说 trade-off 和缺陷（5 分钟）
   cancel_order 目前是 O(N) 线性扫描，O(1) 方案是维护 location map
   position 是单品种，扩展需要改成 map
   没有 TradingEngine 串联——能说清楚为什么没做

5. 数字（3 分钟）
   benchmark 跑出来的 P50/P99，解释测量方法和可信度
```

---

## 五、关键 Q&A 速查

**Q：SPSC 为什么不会有 ABA 问题？**
ABA 问题出现在 CAS（Compare-And-Swap）操作上——值从 A 变 B 再变回 A，CAS 误判为没变化。SPSC 完全不用 CAS，只用 load/store，head 和 tail 都是单调递增的，不存在 ABA。

**Q：epoll ET 比 LT 快在哪？**
减少了 `epoll_wait` 的唤醒次数 = 减少了用户态/内核态切换次数。每次 syscall 约 1μs，在高频行情下这个差异是可测量的。

**Q：为什么 Order 要 fit in one cache line？**
撮合时大量读写 Order 对象。如果 Order 跨两个 cache line，每次访问要 fetch 两条线，吞吐量减半。

**Q：cancel_order 怎么做到 O(1)？**
维护 `unordered_map<order_id, {side, price_index, deque_iterator}>`。cancel 时 O(1) 找到位置，用 iterator 直接从 deque 删除（deque 支持任意位置删除，但需要更新 total_quantity）。

**Q：Risk Gate 的 self-trade check 怎么实现？**
需要一个"我方挂单集合"：每次 submit_order 时记录 `{instrument_id, price, side}`，check 时判断新订单是否会和已有挂单方向相反同价。实现简单，但需要 OrderBook 把"我方订单"和"市场订单"区分开。
