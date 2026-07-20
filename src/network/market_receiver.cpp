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
    // Mock mode for macOS development
    running_ = true;
    // TODO: Read from file/replay for development
#endif
}

void MarketReceiver::stop() noexcept {
    running_ = false;
}

void MarketReceiver::setup_socket() {
#ifdef __linux__
    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

    // Set large receive buffer
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF,
                 &config_.recv_buf_size, sizeof(config_.recv_buf_size));

    // Bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    addr.sin_addr.s_addr = INADDR_ANY;
    ::bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    // Setup epoll (edge-triggered)
    epoll_fd_ = ::epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;  // Edge-triggered
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
        int nfds = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 1 /* 1ms timeout */);
        for (int i = 0; i < nfds; ++i) {
            if (events[i].events & EPOLLIN) {
                // Edge-triggered: read until EAGAIN
                while (true) {
                    ssize_t n = ::recv(socket_fd_, buf, sizeof(buf), 0);
                    if (n <= 0) break;

                    // TODO: Decode market data protocol and call callback_
                    // Timestamp as early as possible
                    // MarketTick tick = decode(buf, n);
                    // tick.local_timestamp_ns = Order::now_ns();
                    // if (callback_) callback_(tick);
                }
            }
        }
    }
#endif
}

}  // namespace minitrader
