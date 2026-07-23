#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace minitrader {

struct MarketTick;  // 前向声明

/// 行情接收器配置。
struct ReceiverConfig {
    std::string multicast_group;       // 组播组 IP（或单播源地址）；
                                       // macOS CSV 回放模式下填 CSV 文件路径
    uint16_t port{9000};               // 监听端口
    int cpu_affinity{-1};              // 绑定 CPU 核（-1 = 不绑定）
    int recv_buf_size{4 * 1024 * 1024}; // Socket 接收缓冲区大小（4MB）；
                                        // CSV 回放模式下复用为回放间隔（微秒）
};

/// 高性能行情接收器，Linux 使用 epoll 边沿触发，macOS 使用 CSV 文件回放。
///
/// 设计要点（Linux 路径）：
/// - 边沿触发 epoll，最小化 syscall 开销
/// - CPU 亲和性绑定，降低跨核 cache 抖动
/// - 大 Socket 缓冲区，吸收突发行情
/// - 尽早打时间戳（在任何处理之前）
/// - 零拷贝解析：直接从接收缓冲区 decode
///
/// macOS / 非 Linux：
/// - ReceiverConfig.multicast_group 设为 CSV 文件路径
/// - 逐行解析 MarketTick 并触发 callback（支持回放间隔控制）
class MarketReceiver {
public:
    using TickCallback = std::function<void(const MarketTick&)>;

    explicit MarketReceiver(ReceiverConfig config);
    ~MarketReceiver();

    // 不可拷贝
    MarketReceiver(const MarketReceiver&) = delete;
    MarketReceiver& operator=(const MarketReceiver&) = delete;

    /// 开始接收行情（阻塞当前线程）。
    /// 每个解码后的 tick 触发一次注册的回调。
    void run();

    /// 停止接收（线程安全，可从其他线程调用）。
    void stop() noexcept;

    /// 注册 tick 回调。
    void set_callback(TickCallback cb) { callback_ = std::move(cb); }

private:
    void setup_socket();      // 创建 UDP socket，设置 epoll（Linux 专用）
    void set_cpu_affinity();  // 绑定 CPU 核（Linux 专用）
    void event_loop();        // epoll 事件循环（Linux 专用）

    ReceiverConfig config_;
    TickCallback   callback_;
    int socket_fd_{-1};
    int epoll_fd_{-1};
    bool running_{false};
};

}  // namespace minitrader
