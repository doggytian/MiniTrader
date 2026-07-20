#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace minitrader {

struct MarketTick;  // Forward declaration

/// Configuration for the market data receiver.
struct ReceiverConfig {
    std::string multicast_group;  // Multicast group IP (or unicast source)
    uint16_t port{9000};
    int cpu_affinity{-1};         // CPU core to pin (-1 = no pinning)
    int recv_buf_size{4 * 1024 * 1024};  // Socket receive buffer (4MB)
};

/// High-performance market data receiver using epoll (edge-triggered).
///
/// Design:
/// - Edge-triggered epoll for minimal syscall overhead
/// - CPU affinity binding to reduce cache thrashing
/// - Large socket buffers to absorb bursts
/// - Timestamping at earliest possible point (before any processing)
/// - Zero-copy parsing: decode directly from the receive buffer
///
/// This class is Linux-only (epoll). On macOS, use kqueue equivalent
/// or compile with -DMINITRADER_MOCK_NETWORK for development.
class MarketReceiver {
public:
    using TickCallback = std::function<void(const MarketTick&)>;

    explicit MarketReceiver(ReceiverConfig config);
    ~MarketReceiver();

    // Non-copyable
    MarketReceiver(const MarketReceiver&) = delete;
    MarketReceiver& operator=(const MarketReceiver&) = delete;

    /// Start receiving market data (blocks the calling thread).
    /// Calls the registered callback for each decoded tick.
    void run();

    /// Stop the receiver (thread-safe, can be called from another thread).
    void stop() noexcept;

    /// Register tick callback.
    void set_callback(TickCallback cb) { callback_ = std::move(cb); }

private:
    void setup_socket();
    void set_cpu_affinity();
    void event_loop();

    ReceiverConfig config_;
    TickCallback callback_;
    int socket_fd_{-1};
    int epoll_fd_{-1};
    bool running_{false};
};

}  // namespace minitrader
