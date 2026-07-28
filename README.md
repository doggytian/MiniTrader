# MiniTrader

[![CI](https://github.com/doggytian/MiniTrader/actions/workflows/ci.yml/badge.svg)](https://github.com/doggytian/MiniTrader/actions/workflows/ci.yml)

用现代 C++20 从零构建的高性能撮合系统，完整实现从行情接收到策略决策到风控落单的全链路，
专注于演示量化交易场景下的低延迟系统设计原则。

## 亮点

- **无锁 SPSC 队列**：`memory_order_acquire/release` 极简内存屏障，行情线程与策略线程之间零争用
- **缓存友好撮合簿**：平坦价格索引数组，无指针追逐，无红黑树
- **事件驱动架构**：行情 → 策略 → 风控 → 落单，单一确定性热路径
- **实测性能**：全链路 P99 延迟经 Google Benchmark 量化，附调用栈 profiling 分析

## 架构

```
┌─────────────┐    SPSC 队列     ┌─────────────┐    风控网关     ┌─────────────┐
│  网络层     │ ───────────────► │  策略引擎   │ ─────────────► │  订单网关   │
│  (epoll)    │  无锁, <50ns     │             │   内联检查      │             │
└─────────────┘                  └─────────────┘                └─────────────┘
       ▲                                                                │
       │                        ┌─────────────┐                        │
       └──── 行情数据 ◄──────── │   交易所    │ ◄──── 回报 ────────────┘
                                └─────────────┘
```

## 模块说明

| 模块 | 描述 | 核心技术 |
|------|------|---------|
| `core/spsc_queue.h` | 无锁单生产者单消费者环形队列 | `std::atomic`、cache-line 填充、`acquire/release` |
| `orderbook/` | 价格-时间优先撮合簿，平坦数组存储 | 缓存友好设计，O(1) 最优买卖价 |
| `engine/` | 交易引擎，串联行情→策略→风控→撮合全链路 | SPSC 驱动，per-tick 延迟计时 |
| `strategy/` | 策略基类 + SpreadStrategy 做市示例 | Template Method 模式，零拷贝事件传递 |
| `network/` | epoll 行情接收器（Linux）/ CSV 回放（macOS） | 边沿触发、非阻塞 IO、CPU 亲和性绑定 |
| `risk/` | 内联风控网关（撤单率、自成交检测、持仓限额） | 零堆分配热路径，O(1) 全部检查 |

## 快速开始

```bash
./scripts/build.sh      # 编译（默认 Release，可传 Debug）
./scripts/run.sh        # 运行交易 demo（含 per-tick 延迟输出）
./scripts/replay.sh     # 真实 tick 序列回放（990000 次样本延迟报告）
./scripts/test.sh       # 运行全量单元测试
./scripts/bench.sh      # 运行全部 benchmark（含 P99 histogram）
./scripts/clean.sh      # 清理 build 目录
```

手动编译：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/minitrader_demo     # 运行 demo
./build/test_spsc_queue     # SPSC 队列测试
./build/test_orderbook      # 撮合簿测试
./build/engine_bench        # 全链路延迟 benchmark
```

## 性能（Apple M1 Pro，Release 构建）

| 指标 | 数值 | 说明 |
|------|------|------|
| SPSC 入队出队往返 | **2.1 ns** | 单线程，int64_t 载荷 |
| SPSC 往返（32 字节 Order 结构体） | **2.5 ns** | |
| SPSC 吞吐（burst 8） | **434 M ops/s** | |
| OrderBook add\_order（单价位） | **43 ns** | |
| OrderBook cancel\_order（O(1)） | **24 ns** | unordered\_map 查找 + list::erase |
| OrderBook 撮合（深度 ≥ 10） | **~450 ns** | 1 笔进场 vs N 笔挂单 |
| **路径A：行情到策略返回（挂单已存在，不下单）** | **~68 ns** | SPSC 出队 + on\_tick 快路径返回 |
| **路径B：行情到双边重新报价（submit×2）** | **~706 ns** | + 2×`RiskGate.check()`(持仓/限频/自成交风控) + 2×`OrderBook.add_order()`(挂单入簿) + 2×`Order::now_ns()`(打时间戳) + 2×`risk_.track_order()`(登记活跃单) |
| **路径B（含 cancel×2）** | **~706 ns** | cancel(`cancel_order`：哈希查 + list::erase) 和 submit(`submit_order`) 耗时相当，差值在误差范围内 |
| **纯策略逻辑（EMA + skew 计算）** | **~2.1 ns** | 绕开 SPSC/OrderBook，直接调 on\_tick |

### 延迟分位数

**① Google Benchmark 手动计时（10 万次稳态采样，含计时开销）**

| P50 | P90 | P99 | P99.9 | 峰值 |
|-----|-----|-----|-------|------|
| 83 ns | 84 ns | 125 ns | 292 ns | ~30 µs |

**② 真实 tick 序列驱动回放（990000 次样本，`./scripts/replay.sh` 可复现）**

| P50 | P99 | P99.9 | 峰值 |
|-----|-----|-------|------|
| 22 ns | 49 ns | 80 ns | ~29 µs |

回放测量的两段分布有业务含义：56% 落在 <25ns（挂单已存在，纯检查路径）；
44% 落在 <50ns（cancel 旧单 + 双边重新报价路径）。

> **计时说明**：Benchmark 手动打两次 `steady_clock::now()` 的开销已包含在测量区间内
>（macOS 上单次调用约数十 ns）。`BM_EngineTickNoOrder`（框架自动计时）的 ~64ns
> 与 `BM_EngineLatencyHistogram` P50=83ns 的差值即为两次手动计时的成本。
> 峰值 ~29 µs 为 OS 调度抖动，非应用逻辑。

### 测量边界与业界差距

本项目的延迟数字衡量的是**纯交易逻辑路径**（SPSC 出队 → 策略 `on_tick` → 风控 → 下单），
属于应用层分解测量的第一段，**并非业界意义的端到端 Tick-to-Trade（T2T，wire-to-wire）**。

| 维度 | 本项目 | 业界生产标准 |
|------|--------|-------------|
| 计时范围 | 出队后 → 下单前 | 网卡收包 → 网卡发包（wire-to-wire） |
| 时间戳来源 | 软件 `steady_clock`（含 OS 抖动） | NIC 硬件时间戳（Solarflare / Mellanox） |
| 核心隔离 | 未绑核 / 未固定频率 | 绑核 + 关超线程 + 固定频率 + 核隔离 |
| 网络栈 | epoll / CSV 回放 | 内核旁路（Onload / DPDK / RDMA） |
| 并发模型 | 单线程 demo | 多核争用 / NUMA 实测 |

因此这些数字用于**横向对比自身优化前后、展示设计 trade-off** 是可信的；但**不可直接对标
生产 T2T 指标**。要接真实对标，需引入 NIC 硬件时间戳做 wire-to-wire，属生产级投入，demo 阶段不做。

## 项目结构

```
MiniTrader/
├── CMakeLists.txt
├── README.md
├── scripts/                    # 一键操作脚本
│   ├── build.sh
│   ├── run.sh
│   ├── test.sh
│   ├── bench.sh
│   └── clean.sh
├── include/
│   ├── core/
│   │   └── spsc_queue.h        # 无锁 SPSC 环形队列
│   ├── orderbook/
│   │   ├── order.h             # 订单类型定义
│   │   ├── price_level.h       # 价位（bid/ask 桶）
│   │   └── order_book.h        # 带撮合引擎的订单簿
│   ├── engine/
│   │   └── trading_engine.h    # 交易引擎（全链路协调）
│   ├── strategy/
│   │   ├── strategy_base.h     # 策略基类接口
│   │   └── spread_strategy.h   # 做市策略示例
│   ├── network/
│   │   └── market_receiver.h   # epoll 行情接收器 / CSV 回放
│   └── risk/
│       └── risk_gate.h         # 内联风控检查
├── src/                        # 对应实现文件
├── tests/                      # 单元测试（Google Test）
├── bench/                      # 微基准测试（Google Benchmark）
├── data/
│   └── sample_ticks.csv        # 示例行情数据（CSV 回放用）
└── docs/                       # 设计文档、profiling 报告
```

## 设计决策

### 为什么用平坦数组而不是 `std::map`？

`std::map`（红黑树）的 O(log N) 访问伴随严重的缓存不友好——每个节点都是独立堆分配，
指针追逐不可避免。对于价格 tick 有界且密集的订单簿，用 `(price - min_price) / tick_size`
作为数组下标实现 O(1) 访问，空间局部性完美。

### 为什么用 SPSC 队列而不是 mutex？

行情线程和策略线程天然构成生产者-消费者对。无锁 SPSC 队列完全消除竞争——通过
cache-line 填充，生产者和消费者永远不会触碰同一 cache line，延迟确定且无 OS 调度依赖。

### 为什么 `cancel_order` 用 `list` 而不是 `deque`？

`deque` 在 `push_back` 时迭代器可能失效，无法缓存"订单位置迭代器"。
`std::list` 迭代器在任意插入/删除后永远有效，配合 `unordered_map<id → iterator>`
实现真正的 O(1) 撤单（哈希查找 + list::erase）。

### 为什么用 epoll 边沿触发？

水平触发的 epoll 只要缓冲区有数据就会在每次 `epoll_wait` 重新通知，增加 syscall 开销。
边沿触发仅在状态变化时通知，配合非阻塞读直到 `EAGAIN`，最小化热路径的内核态切换次数。

## 许可证

MIT
