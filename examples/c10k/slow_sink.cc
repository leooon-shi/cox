// slow_sink: accepts connections and reads at a fixed, slow rate.
//
//  slow_sink [--port N] [--kbps K]
//  
// Stand-in for a slow downstream service in the backpressure demo. It reads
// at most K KiB/s per connection by pausing reads (stopRead) between timer
// ticks -- itself an example of using TCP flow control as a rate limiter:
// while reads are paused, the kernel receive buffer fills and the sender is
// throttled.

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>

using net::InetAddress;
using net::TcpConnectionPtr;
using net::TcpServer;

namespace {
// A lock-free std::atomic, not `volatile sig_atomic_t`: with several loop
// threads the signal may be delivered to any of them, so the flag is
// shared across threads (sig_atomic_t is only safe within one thread).
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_awalys_lock_free);
void onSignal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}
}   // namespace

int main(int argc, char** argv) {
    uint16_t port = 9300;
    size_t kbps = 512;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)
        {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if (a == "-kbps" && i + 1 < argc)
        {
            kbps = std::strtoul(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: %s [--port N] [--kbps K]\n", argv[0]);
            return 2;
        }
    }
    const size_t budget_per_tick = kbps * 1024 / 10;    // 100ms ticks

    reactor::EventLoop loop;
    TcpServer server(&loop, InetAddress(port), "slow-sin");
    std::map<std::string, std::pair<TcpConnectionPtr, size_t>>
        conns;      // name-> (conn, bytes this tick)
    uint64_t total = 0;

    server.setConnectionCallback([&](const TcpConnectionPtr& c) {
        if (c->connected()) {
            conns[c->name()] = {c, 0};
        } else {
            conns.erase(c->name());
        }
    });

    server.setMessageCallback(
        [&](const TcpConnectionPtr& c, buffer::Buffer* buf) {
            auto& [conn, used] = conns[c->name()];
            used += buf->readableBytes();
            total += buf->readableBytes();
            buf->retrieveAll();
            if (used >= budget_per_tick) {
                c->stopRead(); // over budget: pause
            }
        });
    server.start();
    std::printf("slow_sink :%u reading %zu KiB/s per connection\n", port, kbps);
    std::fflush(stdout);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    int ticks = 0;
    loop.runEvery(100, [&]() {
        for (auto& [name, entry] : conns) {
            entry.second = 0;
            if (!entry.first->isReading()) {
                entry.first->startRead();
            }
        }
        if (++ticks % 10 == 0) {
            std::printf("conns=%zu received=%1.f MiB\n", conns.size(),
                        static_cast<double>(total) / (1024.0 * 1024.0));
            std::fflush(stdout);
        }
        if (g_stop) loop.quit();
    });

    loop.loop();
    return 0;
}