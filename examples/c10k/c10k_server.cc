// c10k_server: a tunable echo server for C10K-style experiments.
//
// One binary, every knob a flag, so you can change ONE variable and measure:
//
//   c10k_server [--port N] [--threads N] [--reuseport N] [--max-conns N]
//               [--idle S] [--accept-qps Q] [--nodelay] [--quiet]
//               [--metrics-port N]
//
//   --threads N    N io loops behind ONE listener (main reactor + sub-reactors;
//                  accepted fds are handed out round-robin).
//   --reuseport N  N independent listeners on the same port (SO_REUSEPORT),
//                  each with its own loop thread: the kernel spreads incoming
//                  connections by 4-tuple hash, no shared accept path.
//                  Mutually exclusive with --threads.
//   --max-conns N  connection cap per listener (0 = unlimited).
//   --idle S       close connections idle for S seconds (0 = never).
//   --accept-qps Q token-bucket limit on NEW connections per second.
//   --nodelay      set TCP_NODELAY on accepted connections.
//   --metrics-port N if >0, serve Prometheus metrics at GET /metrics on this
//                  SEPARATE admin port (default 0 = off). The telemetry plane
//                  is kept off the data port so a scraper can always reach
//                  /metrics even while the echo port is under attack -- which
//                  is what the load-validation liveness check relies on.
//   --max-age S    recycle any connection older than S seconds (lifetime cap,
//                  load-validation scenario S6; 0 = off).
//   --write-timeout S  force-close a connection whose output stays backed up
//                  for S seconds (slow-consumer defense, scenario S2; 0 = off).
//   --max_input-buf BYTES  force-close a connection whose unconsumed input
//                  buffer exceeds BYTES (read-side memory bound, scenario S3;
//                  0 = off).
//   --sink         NEVER consume received data (the message callback leaves the
//                  input buffer intact), simulating a protocol parser waiting
//                  for a frame that never completes. Combined with
//                  --max-input-buf this is the S3 target: an unframed flood
//                  accumulates until the input cap force-closes it.
//   --work-ms N    simulate per-request WORK of N ms: each received message is
//                  admitted through a ConcurrencyLimiter and its echo reply is
//                  DEFENDED by N ms (via a timer, so the loop never blocks),
//                  holding an in-flight slot for that long. Scenario S7.
//   --max-inflight N   capacity of the S7 ConcurrencyLimiter (requires
//                  --work-ms). Requests beyong N in flight are SHED immediately
//                  with a cheap "BUSY\n" reply and do not hold a slot.
//
// Prints one stats line per second: live connections, accepted/rejected/closed
// totals and bytes echoed, so you can watch the server while c10k_client (or
// wrk/ab/iperf) drives it. Raise the fd limit first: `ulimit -n 200000`.
//
// Ctrl-C drains and exits.

#include "buffer.h"
#include "concurrency_limiter.h"
#include "event_loop.h"
#include "event_loop_thread.h"
#include "http_response.h"
#include "http_server.h"
#include "inet_address.h"
#include "registry.h"
#include "server_metrics.h"
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
    uint16_t metrics_port = 0;
    uint32_t max_age = 0;        // connection lifetime cap (S6)
    uint32_t write_timeout = 0;  // slow-consumer force-close (S2)
    size_t max_input_buf = 0;    // read-slide memory bound (S3)
    bool sink = false;           // never consume input (S3 target)
    uint32_t work_ms = 0;        // simulated per-request work duration (S7)
    size_t max_inflight = 0;     // concurrency limit for simulated work (S7)
};

struct Stats {  // shared by all loops → atomics
    std::atomic<int64_t> live{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> closed{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> shed{0};  // S7: requests shed over the inflight cap
};

[[noreturn]] void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--threads N | --reuseport N] "
                 "[--max-conns N] [--idle S] [--accept-qps Q] [--nodelay] "
                 "[--quiet] [--metrics-port N] [--max-age S] "
                 "[--write-timeout S] [--max-input-buf BYTES] [--sink] "
                 "[--work-ms N] [--max-inflight N]\n",
                 prog);
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage(argv[0]);
            }
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
            o.idle = static_cast<uint32_t>(std::atoi(val()));
        } else if (a == "--accept-qps") {
            o.accept_qps = std::atof(val());
        } else if (a == "--nodelay") {
            o.nodelay = true;
        } else if (a == "--quiet") {
            o.quiet = true;
        } else if (a == "--metrics-port") {
            o.metrics_port = static_cast<uint16_t>(std::atoi(val()));
        } else if (a == "--max-age") {
            o.max_age = static_cast<uint32_t>(std::atoi(val()));
        } else if (a == "--write-timeout") {
            o.write_timeout = static_cast<uint32_t>(std::atoi(val()));
        } else if (a == "--max-input-buf") {
            o.max_input_buf = std::strtoul(val(), nullptr, 10);
        } else if (a == "--sink") {
            o.sink = true;
        } else if (a == "--work-ms") {
            o.work_ms = static_cast<uint32_t>(std::atoi(val()));
        } else if (a == "--max-inflight") {
            o.max_inflight = std::strtoul(val(), nullptr, 10);
        } else {
            usage(argv[0]);
        }
    }
    if (o.threads > 0 && o.reuseport > 0) {
        usage(argv[0]);
    }
    return o;
}

// Configure one TcpServer as an echo endpoint that updates the shared stats.
// When `reg` is non-null, also install the standard Cox server metrics on it
// (chained AFTER the echo callbacks, as installServerMetrics requires). A
// single shared Registry across all listeners is safe: the metric atomics and
// the helper's dedup set are thread safe, so N reuseport loops aggregate into
// one metric set.
void configure(TcpServer& server, const Options& o, Stats& st,
               metrics::Registry* reg,
               std::shared_ptr<flowcontrol::ConcurrencyLimiter> limiter) {
    if (o.threads > 0) {
        server.setThreadNum(o.threads);
    }
    if (o.max_conns > 0) {
        server.setMaxConnections(o.max_conns);
    }
    if (o.idle > 0) {
        server.setIdleTimeout(o.idle);
    }
    if (o.max_age > 0) {
        server.setMaxConnectionAge(o.max_age);
    }
    if (o.write_timeout > 0) {
        server.setWriteTimeout(o.write_timeout);
    }
    if (o.max_input_buf > 0) {
        server.setMaxInputBufferSize(o.max_input_buf);
    }
    if (o.accept_qps > 0) {
        server.setAcceptRateLimit(o.accept_qps, o.accept_qps);
    }
    server.setOverloadCallback([&st](const InetAddress&) { ++st.rejected; });
    const bool nodelay = o.nodelay;
    server.setConnectionCallback([&st, nodelay](const TcpConnectionPtr& c) {
        if (c->connected()) {
            ++st.accepted;
            ++st.live;
            if (nodelay) {
                c->setTcpNoDelay(true);
            }
        } else {
            ++st.closed;
            --st.live;
        }
    });
    server.setMessageCallback(
        [&st, sink = o.sink, work_ms = o.work_ms, limiter](
            const TcpConnectionPtr& c, buffer::Buffer* buf) {
            if (sink) {
                // S3 target: a "parser" that never completes a frame, so it
                // NEVER consumes the input buffer. Unconsumed bytes accumulate
                // until the max-input-buffer cap force-closes the connection.
                // Do NOT retrieve/echo (leaving the data is the point) and do
                // NOT add buf->readableBytes() to st.bytes -- the buffer is not
                // drained, so it would re-count the whole backlog every read.
                return;
            }
            if (limiter) {
                // S7: simulated slow work under a concurrency limit. Admit via
                // the limiter; on success hold the permit while the reply is
                // DEFERRED by work_ms (a timer, so the loop never blocks), then
                // echo and release. Over capacity -> shed immediately with a
                // cheap reply, holding no slot.
                std::string data = buf->retrieveAllAsString();
                auto permit = limiter->tryAcquire();
                if (!permit) {
                    ++st.shed;
                    c->send("BUSY\n");
                    return;
                }
                st.bytes += data.size();
                // Move the permit into the deferred task so the slot is held
                // for the whole simulated work duration, then released on
                // completion. Capture the connection shared_ptr to keep it
                // alive across the delay.
            auto p = std::make_shared<flowcontrol::ConcurrencyLimiter::Permit>(
                std::move(permit);
            c->getLoop()->runAfter(work_ms, [c, data = std::move(data), p]() {
                c->send(data);
                p->release();  // work done: free the in-flight slot
            });
            return;
            }
            st.bytes += buf->readableBytes();
            c->send(buf->retrieveAllAsString());
        });
    // Install metrics LAST so the helper chains onto the echo callbacks above
    // rather than being clobbered by them.
    if (reg != nullptr) {
        metrics::installServerMetrics(*reg, server);
    }
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

    // One shared registry across all listeners (thread-safe). Only created when
    // metrics are requested so the no-metrics path pays nothing.
    std::unique_ptr<metrics::Registry> registry;
    if (o.metrics_port > 0) {
        registry = std::make_unique<metrics::Registry>();
    }
    metrics::Registry* reg = registry.get();

    // S7: a shared ConcurrencyLimiter for simulated slow work, created only
    // when --work-ms is set. Shared across all listeners so the in-flight cap
    // is global (reuseport loops share one budge). The message callbacks hold
    // a copy, so it outlives any deferred work task.
    std::shared_ptr<flowcontrol::ConcurrencyLimiter> limiter;
    if (o.work_ms > 0) {
        limiter =
            std::make_shared<flowcontrol::ConcurrencyLimiter>(o.max_inflight);
    }
    if (reg != nullptr && limiter) {
        // Sampled in-flight gauge + a shed counter, so S7 can assert the gauge
        // stays <= capacity and excess is shed.
        auto* lim = limiter.get();
        reg->registerSampled(
            "cox_request_in_flight", "Requests currently in flight (S7 work)",
            metrics::MetricPoint::Type::kGauge,
            [lim] { return static_cast<double>(lim->inFlight()); });
        Stats* stp = &st;
        reg->registerSampled("cox_requests_shed_total",
                             "Requests shed over the in-flight capacity (S7)",
                             metrics::MetricPoint::Type::kCounter, [stp] {
                                 return static_cast<double>(stp->shed.load());
                             });
    }

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
                configure(*srv, o, st, reg, limiter);
                srv->start();
                ready = true;
            });
            while (!ready) {
                std::this_thread::yield();
            }
            servers.emplace_back(srv);
            listener_threads.push_back(std::move(t));
        }
        std::printf("c10k_server: %d SO_REUSEPORT listeners on :%u\n",
                    o.reuseport, o.port);
    } else {
        servers.push_back(
            std::make_unique<TcpServer>(&main_loop, addr, "c10k"));
        configure(*servers.back(), o, st, reg, limiter);
        servers.back()->start();
        std::printf("c10k_server: 1 listener + %d io threads on :%u\n",
                    o.threads, o.port);
    }

    // Admin/telemetry plane: a SEPARATE HttpServer on the main loop serving
    // GET /metrics in Prometheus text format. Kept off the data port so the
    // scraper can always reach it, even while the echo port is saturated by an
    // attack (the load-validation liveness measurement depends on this).
    std::unique_ptr<http::HttpServer> metrics_server;
    if (reg != nullptr) {
        metrics::installLoopMetrics(*reg, main_loop, {{"loop", "main"}});
        metrics_server = std::make_unique<http::HttpServer>(
            &main_loop, InetAddress(o.metrics_port), "c10k-metrics");
        metrics_server->setHttpHandler(
            [reg](const http::HttpRequest& req, http::HttpResponse* resp) {
                if (req.path() != "/metrics") {
                    resp->setStatus(404);
                    resp->setBody("not found\n");
                    return;
                }
                resp->setStatus(200);
                resp->setContentType("text/plain; version=0.0.4");
                resp->setBody(metrics::expo::prometheus(reg->scrape()));
            });
        metrics_server->start();
        std::printf("c10_server: metrics on http://127.0.0.1:%u/metrics\n",
                    o.metrics_port);
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
            while (!done) {
                std::this_thread::yield();
            }
        }
        listener_threads.clear();  // quits and joins each loop thread
    }
    std::printf("stopped: accepted=%llu rejected=%llu\n",
                static_cast<unsigned long long>(st.accepted.load()),
                static_cast<unsigned long long>(st.rejected.load()));
    return 0;
}
