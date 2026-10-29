// backpressure_proxy: a TCP relay that demonstrates end-to-end backpressure.
//
//   backpressure_proxy [--listen PORT] [--upstream PORT] [--hwm BYTES]
//                      [--no-backpressure]
//
// Each client connection gets its own upstream connection to
// 127.0.0.1:--upstream. Bytes are relayed both ways.
//
// The interesting case is a FAST sender and a SLOW receiver. Without
// backpressure the proxy keeps reading from the fast side and queues
// everything in user space: memory grows without bound (watch RSS). With
// backpressure, when the outgoing side's output buffer passes the high-water
// mark (--hwm) the proxy calls stopRead() on the incoming side; the kernel
// receive buffer fills, TCP's window closes, and the fast sender is throttled
// by TCP itself. Reading resumes from the write-complete callback once the
// slow side drains.
//
// Try it (see examples/README.md for the full walkthrough):
//   slow sink:   ./slow_sink --port 9300 --kbps 512
//   proxy:       ./backpressure_proxy --listen 9200 --upstream 9300
//   fast source: head -c 200M /dev/zero | nc -q1 127.0.0.1 9200
//   watch:       the proxy prints queued bytes + RSS every second; compare
//                with --no-backpressure.

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_client.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <string>

using net::InetAddress;
using net::TcpClient;
using net::TcpConnectionPtr;
using net::TcpServer;

namespace {
// A lock-free std::atomic, not `volatile sig_atomic_t`: with several loop
// threads the signal may be delivered to any of them, so the flag is
// shared across threads (sig_atomic_t is only safe within one thread).
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void onSignal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

long rssKb() {
    std::ifstream f("/proc/self/status");
    std::string k;
    long v = 0;
    while (f >> k) {
        if (k == "VmRSS:") {
            f >> v;
            return v;
        }
    }
    return 0;
}

// Link the two halves of a relay so that (a) each forwards to the other and
// (b) each can pause/resume the other's reads.
void pipe(const TcpConnectionPtr& from, const TcpConnectionPtr& to, size_t hwm,
          bool backpressure) {
    from->setMessageCallback(
        [weak_to = std::weak_ptr<net::TcpConnection>(to)](
            const TcpConnectionPtr& self, buffer::Buffer* buf) {
            if (auto t = weak_to.lock()) {
                t->send(buf->retrieveAllAsString());
            } else {
                buf->retrieveAll();
                self->shutdown();
            }
        });
    if (!backpressure) return;
    // When `to` backs up past hwm, stop reading from `from`.
    to->setHighWaterMarkCallback(
        [weak_from = std::weak_ptr<net::TcpConnection>(from)](
            const TcpConnectionPtr&, size_t) {
            if (auto f = weak_from.lock()) f->stopRead();
        },
        hwm);
    // When `to` has flushed everything, resume reading from `from`.
    to->setWriteCompleteCallback([weak_from = std::weak_ptr<net::TcpConnection>(
                                      from)](const TcpConnectionPtr&) {
        if (auto f = weak_from.lock()) {
            if (!f->isReading()) f->startRead();
        }
    });
}
}  // namespace

int main(int argc, char** argv) {
    uint16_t listen_port = 9200, upstream_port = 9300;
    size_t hwm = 1 << 20;  // 1 MiB
    bool backpressure = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--listen" && i + 1 < argc) {
            listen_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (a == "--upstream" && i + 1 < argc) {
            upstream_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (a == "--hwm" && i + 1 < argc) {
            hwm = std::strtoul(argv[++i], nullptr, 10);
        } else if (a == "--no-backpressure") {
            backpressure = false;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--listen P] [--upstream P] [--hwm BYTES] "
                         "[--no-backpressure]\n",
                         argv[0]);
            return 2;
        }
    }

    reactor::EventLoop loop;
    TcpServer server(&loop, InetAddress(listen_port), "bp-proxy");
    // client connection name -> its upstream TcpClient (owned here, loop
    // thread)
    std::map<std::string, std::unique_ptr<TcpClient>> upstreams;

    server.setConnectionCallback([&](const TcpConnectionPtr& down) {
        if (down->connected()) {
            down->stopRead();  // don't read until the upstream is connected
            auto client = std::make_unique<TcpClient>(
                &loop, InetAddress("127.0.0.1", upstream_port),
                "upstream-" + down->name());
            client->setConnectionCallback(
                [&, weak_down = std::weak_ptr<net::TcpConnection>(down)](
                    const TcpConnectionPtr& up) {
                    auto d = weak_down.lock();
                    if (up->connected()) {
                        if (!d) {
                            up->shutdown();
                            return;
                        }
                        pipe(d, up, hwm, backpressure);  // client -> upstream
                        pipe(up, d, hwm, backpressure);  // upstream -> client
                        d->startRead();
                    } else if (d) {
                        d->shutdown();  // upstream gone: half-close the client
                    }
                });
            client->connect();
            upstreams[down->name()] = std::move(client);
        } else {
            auto it = upstreams.find(down->name());
            if (it != upstreams.end()) {
                // Destroy the TcpClient outside its own callback chain.
                TcpClient* c = it->second.release();
                upstreams.erase(it);
                c->disconnect();
                loop.queueInLoop([c]() { delete c; });
            }
        }
    });

    server.start();
    std::printf("backpressure_proxy :%u -> :%u  backpressure=%s hwm=%zu\n",
                listen_port, upstream_port, backpressure ? "on" : "OFF", hwm);
    std::fflush(stdout);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    loop.runEvery(1000, [&]() {
        std::printf("pairs=%zu rss=%ld KiB\n", upstreams.size(), rssKb());
        std::fflush(stdout);
        if (g_stop) loop.quit();
    });

    loop.runEvery(100, [&]() {
        if (g_stop) loop.quit();
    });

    loop.loop();
    return 0;
}
