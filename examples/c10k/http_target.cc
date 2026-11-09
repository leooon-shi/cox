// http_target: a tunable HTTP/1.1 server for load-validation scenario S4
// (HTTP body limit vs big-body). It is the HTTP analogue of c10k_server: every
// knob is a flag, and it exposes Prometheus /metrics (including
// cox_http_requests_total{status_class=...}) on a SEPARATE admin port so a
// scrape is reachable even while the data port is under attack.
//
//   http_target [--port N] [--metrics-port N] [--max-body BYTES] [--quiet]
//
//   --port N          HTTP data port (default 9100).
//   --metrics-port N  if >0, serve Prometheus /metrics on this admin port.
//   --max-body BYTES  reject request bodies larger than BYTES with 413 (0 =
//                     the HttpServer default). A Content-Length over the cap is
//                     refused UP FRONT without reading the body; a chunked body
//                     is rejected once the running total exceeds the cap.
//   --quiet           suppress the per-second stats line.
//
// Handler: POST /sink consumes and discards the body, replies 200 with the
// received length; any oversized body never reaches the handler (the parser
// rejects it with 413 first). GET / replies 200. This lets S4 drive a big-body
// POST and watch the 413 + bounded memory, while small requests keep 2xx-ing.
//
// Links cox::metrics (-> cox::http -> cox::net).

#include "event_loop.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"
#include "inet_address.h"
#include "registry.h"
#include "server_metrics.h"

#include <sys/resource.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using http::HttpServer;
using net::InetAddress;

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

struct Options {
    uint16_t port = 9100;
    uint16_t metrics_port = 0;
    size_t max_body = 0;
    bool quiet = false;
};

[[noreturn]] void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--metrics-port N] [--max-body BYTES] "
                 "[--quiet]\n",
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
        } else if (a == "--metrics-port") {
            o.metrics_port = static_cast<uint16_t>(std::atoi(val()));
        } else if (a == "--max-body") {
            o.max_body = std::strtoul(val(), nullptr, 10);
        } else if (a == "--quiet") {
            o.quiet = true;
        } else {
            usage(argv[0]);
        }
    }
    return o;
}

void raiseFdLimit(int want) {
    rlimit rl{};
    ::getrlimit(RLIMIT_NOFILE, &rl);
    rlim_t need = static_cast<rlim_t>(want) + 64;
    if (rl.rlim_cur < need) {
        rl.rlim_cur = std::min(need, rl.rlim_max);
        ::setrlimit(RLIMIT_NOFILE, &rl);
    }
}
}  // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    raiseFdLimit(20000);

    reactor::EventLoop loop;
    HttpServer server(&loop, InetAddress(o.port), "http-target");
    if (o.max_body > 0) {
        server.setMaxBodySize(o.max_body);
    }

    server.setHttpHandler(
        [](const http::HttpRequest& req, http::HttpResponse* resp) {
            // Any oversized body was already rejected (413) by the parser and
            // never reaches here. POST /sink consumes+discards and reports the
            // length; everything else is a trivial 200.
            if (req.method() == http::Method::kPost && req.path() == "/sink") {
                resp->setStatus(200);
                resp->setContentType("text/plain");
                resp->setBody("received " + std::to_string(req.body().size()) +
                              " bytes\n");
            } else {
                resp->setStatus(200);
                resp->setContentType("text/plain");
                resp->setBody("ok\n");
            }
        });

    // Telemetry plane on a separate admin port, like c10k_server.
    std::unique_ptr<metrics::Registry> registry;
    std::unique_ptr<HttpServer> metrics_server;
    if (o.metrics_port > 0) {
        registry = std::make_unique<metrics::Registry>();
        // cox_http_requests_total{status_class=...} (incl. 4xx for the 413s)
        // and request-duration histogram, via the public RequestObserver hook.
        metrics::installHttpMetrics(*registry, server);
        metrics::installLoopMetrics(*registry, loop, {{"loop", "main"}});
        metrics::Registry* reg = registry.get();
        metrics_server = std::make_unique<HttpServer>(
            &loop, InetAddress(o.metrics_port), "http-target-metrics");
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
        std::printf("http_target: metrics on http://127.0.0.1:%u/metrics\n",
                    o.metrics_port);
    }

    server.start();
    std::printf("http_target: HTTP on :%u (max-body=%zu)\n", o.port,
                o.max_body);
    std::fflush(stdout);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    loop.runEvery(100, [&loop]() {
        if (g_stop) {
            loop.quit();
        }
    });
    loop.loop();
    std::printf("stopped\n");
    return 0;
}
