// c10k_server: a tunable echo server for C10K-style experiments.
//
// One binary, every knob a flag, so you can change ONE variable and measure:
//
//   c10k_server [--port N] [--threads N] [--reuseport N] [--max-conns N]
//               [--idle S] [--accept-qps Q] [--nodelay] [--quiet]
//
//   --threads N    N io loops behind ONE listener (main reactor + sub-reactors;
//                  accepted fds are handed out round-robin).
//   --reuseport N  N independent listeners on the same port (SO_REUSEPORT),
//                  each with its own own loop thread: the kernel spreads
//                  incoming connections by 4-tuple hash, no shared accept path.
//                  Mutually exclusive with --threads.
//   --max-conns N  connection cap per listener (0 == unlimited).
//   --idle S       close connections idle for S seconds (0 = never).
//   --accept-qps Q token-bucket limit on NEW connections per second.
//   --nodelay      set TCP_NODELAY on accepted connections.
//
// Prints one stats line per second: live connections, accepted/rejected/closed
// totals and bytes echoed, so you can watch the server while c10k_client (or
// wrk/ab/iperf) drives it. Raise the fd limit first: `ulimit -n 200000`.
//
// Ctrl-C drains and exits.

#include "buffer.h"
#include "event_loop.h"
#include "event_loop_thread.h"
#include "inet_address.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include <sys/resource.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using net::InetAddress;
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

struct Options {
    uint16_t port = 9100;
    int threads = 0;
    int reuseport = 0;
    size_t max_conns = 0;
    uint32_t idle = 0;
    double accept_qps = 0;
    bool nodelay = false;
    bool quiet = false;
};

struct Stats {  // shared by all loops → atomics
    std::atomic<int64_t> live{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> closed{0};
    std::atomic<uint64_t> bytes{0};
};

[[noreturn]] void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--threads N | --reuseport N] "
                 "[--max-conns N] [--idle S] [--accept-qps Q] [--nodelay] "
                 "[--quiet]\n",
                 prog);
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) usage(argv[0]);
            return argv[++i];
        };
        if (a == "--port") {
            o.port = static_cast<uint16_t>(std::atoi(val()));
        } else if (a == "--threads") {
            o.threads = std::atoi(val());
        } else if (a == "--reuseport") {
            o.reuseport = std::atoi(val());
        } else if (a == "--max-conns") {
            o.max_conns = std::strtoul(val(), nullptr, 10);
        } else if (a == "--idle") {
            o.idle = static_cast<uint32_t>(std::atof(val()));
        } else if (a == "--accept-qps") {
            o.accept_qps = std::atof(val());
        } else if (a == "--nodelay") {
            o.nodelay = true;
        } else if (a == "--quiet") {
            o.quiet = true;
        } else {
            usage(argv[0]);
        }
    }
    if (o.threads > 0 && o.reuseport > 0) usage(argv[0]);
    return o;
}

// Configure one TcpServer as an echo endpoint that updates the shared stats.
void configure(TcpServer& server, const Options& o, Stats& st) {
    if (o.threads > 0) server.setThreadNum(o.threads);
    if (o.max_conns > 0) server.setMaxConnections(o.max_conns);
    if (o.idle > 0) server.setIdleTimeout(o.idle);
    if (o.accept_qps > 0) server.setAcceptRateLimit(o.accept_qps, o.accept_qps);
    server.setOverloadCallback([&st](const InetAddress&) { ++st.rejected; });
    const bool nodelay = o.nodelay;
    server.setConnectionCallback([&st, nodelay](const TcpConnectionPtr& c) {
        if (c->connected()) {
            ++st.accepted;
            ++st.live;
            if (nodelay) c->setTcpNoDelay(true);
        } else {
            ++st.closed;
            --st.live;
        }
    });
    server.setMessageCallback(
        [&st](const TcpConnectionPtr& c, buffer::Buffer* buf) {
            st.bytes += buf->readableBytes();
            c->send(buf->retrieveAllAsString());
        });
}

void printFdLimit() {
    rlimit rl{};
    ::getrlimit(RLIMIT_NOFILE, &rl);
    std::printf("RLIMIT_NOFILE soft=%llu hard=%llu",
                static_cast<unsigned long long>(rl.rlim_cur),
                static_cast<unsigned long long>(rl.rlim_max));
    if (rl.rlim_cur < 20000) {
        std::printf("  (low: run `ulimit -n 200000` for C10K tests)");
    }
    std::printf("\n");
}
}  // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    Stats st;
    printFdLimit();

    reactor::EventLoop main_loop;
    InetAddress addr(o.port);

    // Mode A: one listener (optionally with N io threads).
    // Mode B: N SO_REUSEPORT listeners, each owning its loop thread.
    std::vector<std::unique_ptr<TcpServer>> servers;
    std::vector<std::unique_ptr<reactor::EventLoopThread>> listener_threads;

    if (o.reuseport > 0) {
        for (int i = 0; i < o.reuseport; ++i) {
            auto t = std::make_unique<reactor::EventLoopThread>(
                nullptr, "reuseport-" + std::to_string(i));
            reactor::EventLoop* loop = t->startLoop();
            // TcpServer must be created, started and destroyed on its own loop.
            TcpServer* srv = nullptr;
            std::atomic<bool> ready{false};
            loop->runInLoop([&, loop, i]() {
                srv = new TcpServer(loop, addr, "c10k-rp" + std::to_string(i),
                                    TcpServer::Option::kReusePort);
                configure(*srv, o, st);
                srv->start();
                ready = true;
            });
            while (!ready) std::this_thread::yield();
            servers.emplace_back(srv);
            listener_threads.push_back(std::move(t));
        }
        std::printf("c10k_server: %d SO_REUSEPORT listeners on :%u\n",
                    o.reuseport, o.port);
    } else {
        servers.push_back(
            std::make_unique<TcpServer>(&main_loop, addr, "c10k"));
        configure(*servers.back(), o, st);
        servers.back()->start();
        std::printf("c10k_server: 1 listener + %d io threads on :%u\n",
                    o.threads, o.port);
    }
    std::fflush(stdout);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    uint64_t last_bytes = 0;
    main_loop.runEvery(1000, [&]() {
        if (!o.quiet) {
            uint64_t b = st.bytes.load();
            std::printf(
                "live=%lld accepted=%llu rejected=%llu closed=%llu "
                "echo=%.1f MB/s\n",
                static_cast<long long>(st.live.load()),
                static_cast<unsigned long long>(st.accepted.load()),
                static_cast<unsigned long long>(st.rejected.load()),
                static_cast<unsigned long long>(st.closed.load()),
                static_cast<double>(b - last_bytes) / (1024.0 * 1024.0));
            last_bytes = b;
            std::fflush(stdout);
        }
    });
    main_loop.runEvery(100, [&]() {
        if (g_stop) {
            main_loop.quit();
        }
    });
    main_loop.loop();

    // Tear down: SO_REUSEPORT servers are destroyed on their own loops.
    if (o.reuseport > 0) {
        for (size_t i = 0; i < servers.size(); ++i) {
            TcpServer* srv = servers[i].release();
            std::atomic<bool> done{false};
            srv->getLoop()->runInLoop([srv, &done]() {
                delete srv;
                done = true;
            });
            while (!done) std::this_thread::yield();
        }
        listener_threads.clear();  // quits and joins each loop thread
    }
    std::printf("stopped: accepted=%llu rejected=%llu\n",
                static_cast<unsigned long long>(st.accepted.load()),
                static_cast<unsigned long long>(st.rejected.load()));
    return 0;
}
