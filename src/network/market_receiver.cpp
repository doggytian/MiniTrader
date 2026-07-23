#include "network/market_receiver.h"
#include "strategy/strategy_base.h"

// Platform-specific includes
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
    // ── macOS / non-Linux: CSV replay mode ──────────────────────────────────
    // Expected CSV format (header required):
    //   instrument_id,bid_price,ask_price,bid_size,ask_size,last_price,last_size
    //
    // Example:
    //   1,9998,10002,100,100,10000,10
    //   1,9997,10003,200,150,10000,5
    //
    // If config_.multicast_group is a readable file path, replay it;
    // otherwise print a warning and return immediately.

    const std::string& path = config_.multicast_group;
    if (path.empty()) {
        std::fprintf(stderr,
            "[MarketReceiver] macOS: set ReceiverConfig.multicast_group to a "
            "CSV file path for replay mode.\n");
        return;
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr,
            "[MarketReceiver] replay: cannot open '%s'\n", path.c_str());
        return;
    }

    running_ = true;
    std::string line;

    // Skip header line if present
    if (std::getline(f, line)) {
        // Detect header: if first token is not a number, it's a header row
        bool is_header = (line[0] < '0' || line[0] > '9');
        if (!is_header) {
            // Re-parse this line as data
            std::istringstream ss(line);
            MarketTick tick{};
            char comma;
            if (ss >> tick.instrument_id >> comma
                   >> tick.bid_price    >> comma
                   >> tick.ask_price    >> comma
                   >> tick.bid_size     >> comma
                   >> tick.ask_size     >> comma
                   >> tick.last_price   >> comma
                   >> tick.last_size) {
                tick.exchange_timestamp_ns = Order::now_ns();
                tick.local_timestamp_ns    = Order::now_ns();
                if (callback_) callback_(tick);
            }
        }
    }

    while (running_ && std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;

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
                "[MarketReceiver] replay: malformed line: %s\n", line.c_str());
            continue;
        }

        tick.exchange_timestamp_ns = Order::now_ns();
        tick.local_timestamp_ns    = Order::now_ns();

        if (callback_) callback_(tick);

        // Simulate ~1ms between ticks (configurable via recv_buf_size field
        // repurposed as replay_interval_us when in replay mode)
        if (config_.recv_buf_size > 0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(config_.recv_buf_size));
        }
    }
    running_ = false;
#endif
}

void MarketReceiver::stop() noexcept {
    running_ = false;
}

void MarketReceiver::setup_socket() {
#ifdef __linux__
    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

    ::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF,
                 &config_.recv_buf_size, sizeof(config_.recv_buf_size));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    addr.sin_addr.s_addr = INADDR_ANY;
    ::bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    epoll_fd_ = ::epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
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
        int nfds = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 1);
        for (int i = 0; i < nfds; ++i) {
            if (events[i].events & EPOLLIN) {
                while (true) {
                    ssize_t n = ::recv(socket_fd_, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    // TODO: decode wire protocol → MarketTick → callback_
                    (void)n;
                }
            }
        }
    }
#endif
}

}  // namespace minitrader
