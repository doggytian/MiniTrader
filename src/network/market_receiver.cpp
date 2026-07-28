#include "network/market_receiver.h"
#include "strategy/strategy_base.h"

// 平台相关头文件
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "orderbook/order.h"

namespace minitrader {

MarketReceiver::MarketReceiver(ReceiverConfig config)
    : config_(std::move(config))
{
}

MarketReceiver::~MarketReceiver() {
#ifdef __linux__
    if (socket_fd_ >= 0) ::close(socket_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
#endif
}

void MarketReceiver::run() {
#ifdef __linux__
    setup_socket();
    if (config_.cpu_affinity >= 0) {
        set_cpu_affinity();
    }
    running_ = true;
    event_loop();
#else
    // ── macOS / 非 Linux：CSV 文件回放模式 ──────────────────────────────────
    // CSV 格式（支持可选表头行）：
    //   instrument_id,bid_price,ask_price,bid_size,ask_size,last_price,last_size
    //
    // 示例：
    //   1,9998,10002,100,100,10000,10
    //   1,9997,10003,200,150,10000,5
    //
    // config_.multicast_group 填 CSV 文件路径；
    // config_.recv_buf_size 复用为回放间隔（微秒，0 = 不等待）。

    const std::string& path = config_.multicast_group;
    if (path.empty()) {
        std::fprintf(stderr,
            "[MarketReceiver] macOS 回放模式：请将 ReceiverConfig.multicast_group "
            "设为 CSV 文件路径（参考 data/sample_ticks.csv）\n");
        return;
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr,
            "[MarketReceiver] 无法打开回放文件：%s\n", path.c_str());
        return;
    }

    running_ = true;
    std::string line;

    // 回放基准：记录第一条 tick 的 exchange_timestamp_ns 和本地时钟，
    // 后续每条 tick 根据相对时间差决定是否 sleep（按真实间隔回放）。
    bool     first_tick        = true;
    uint64_t base_exchange_ns  = 0;
    uint64_t base_local_ns     = 0;

    while (running_ && std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;  // 跳过空行和注释
        // 跳过表头行（首字符为字母）
        if (line[0] < '0' || line[0] > '9') continue;

        std::istringstream ss(line);
        MarketTick tick{};
        char comma;
        if (!(ss >> tick.instrument_id >> comma
                 >> tick.bid_price    >> comma
                 >> tick.ask_price    >> comma
                 >> tick.bid_size     >> comma
                 >> tick.ask_size     >> comma
                 >> tick.last_price   >> comma
                 >> tick.last_size)) {
            std::fprintf(stderr,
                "[MarketReceiver] 格式错误，跳过：%s\n", line.c_str());
            continue;
        }

        // 尝试解析可选的 exchange_timestamp_ns 列（向后兼容旧格式）
        uint64_t exchange_ts = 0;
        char comma2 = 0;
        if (ss >> comma2 >> exchange_ts) {
            tick.exchange_timestamp_ns = exchange_ts;
        } else {
            tick.exchange_timestamp_ns = Order::now_ns();
        }

        // 按真实 tick 间隔回放：首条 tick 建立时钟基准，后续按偏移量 sleep
        if (first_tick) {
            base_exchange_ns = tick.exchange_timestamp_ns;
            base_local_ns    = Order::now_ns();
            first_tick       = false;
        } else if (tick.exchange_timestamp_ns > base_exchange_ns) {
            // 计算该 tick 应在何时到达（基于本地基准时钟）
            const uint64_t target_local_ns =
                base_local_ns + (tick.exchange_timestamp_ns - base_exchange_ns);
            const uint64_t now = Order::now_ns();
            if (target_local_ns > now) {
                std::this_thread::sleep_for(
                    std::chrono::nanoseconds(target_local_ns - now));
            }
        }

        // local_timestamp_ns：sleep 结束后、callback 前立即打时间戳，
        // 模拟行情到达本地的时刻；local - exchange = 端到端传输延迟（模拟网络时延）
        tick.local_timestamp_ns = Order::now_ns();

        if (callback_) callback_(tick);
    }
    running_ = false;
#endif
}

void MarketReceiver::stop() noexcept {
    running_ = false;
}

void MarketReceiver::setup_socket() {
#ifdef __linux__
    // 创建非阻塞 UDP socket
    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

    // 设置大接收缓冲区，吸收突发行情
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF,
                 &config_.recv_buf_size, sizeof(config_.recv_buf_size));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(config_.port);
    addr.sin_addr.s_addr = INADDR_ANY;
    ::bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    // 注册到 epoll，边沿触发（ET）
    epoll_fd_ = ::epoll_create1(0);
    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = socket_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket_fd_, &ev);
#endif
}

void MarketReceiver::set_cpu_affinity() {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(config_.cpu_affinity, &cpuset);
    ::sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#endif
}

void MarketReceiver::event_loop() {
#ifdef __linux__
    constexpr int MAX_EVENTS = 16;
    epoll_event events[MAX_EVENTS];
    char buf[65536];

    while (running_) {
        // 1ms 超时，允许 stop() 后及时退出
        int nfds = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 1);
        for (int i = 0; i < nfds; ++i) {
            if (events[i].events & EPOLLIN) {
                // 边沿触发：循环读直到 EAGAIN
                while (true) {
                    ssize_t n = ::recv(socket_fd_, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    // TODO: 解码行情协议 → MarketTick → callback_
                    (void)n;
                }
            }
        }
    }
#endif
}

}  // namespace minitrader
