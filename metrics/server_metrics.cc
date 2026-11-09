#include "server_metrics.h"

#include "event_loop.h"
#include "http_server.h"
#include "inet_address.h"
#include "resolver.h"
#include "tcp_connection.h"
#include "tcp_server.h"
#include "thread_pool.h"
#include "tls_context.h"

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace metrics {

void installServerMetrics(Registry& reg, net::TcpServer& server) {
    // Register instruments once; the registry owns them and returns stable
    // references captured by the callbacks below.
    Counter& accepted = reg.counter("cox_connections_accepted_total",
                                    "Total connections accepted by the server");
    // Per-reason close counters (one metric name, distinct 'reason' label), so
    // a scrape attributes closes to the specific defense that fired. The reason
    // comes from TcpConnection::closeReason(), set at each force-close site.
    // Pre-created so the disconnect path only does inc(), no lookup. There is
    // NO unlabeled aggregate: the total is sum(by reason), and mixing an
    // unlabeled series with labeled ones under one name is malformed Prometheus
    // (mirrors cox_http_requests_total, which is only ever labeled).
    Counter& closed_peer =
        reg.counter("cox_connections_closed_total", "Total connections closed",
                    {{"reason", "peer"}});
    Counter& closed_idle =
        reg.counter("cox_connections_closed_total", "Total connections closed",
                    {{"reason", "idle"}});
    Counter& closed_write =
        reg.counter("cox_connections_closed_total", "Total connections closed",
                    {{"reason", "write_timeout"}});
    Counter& closed_maxage =
        reg.counter("cox_connections_closed_total", "Total connections closed",
                    {{"reason", "max_age"}});
    Counter& closed_inputcap =
        reg.counter("cox_connections_closed_total", "Total connections closed",
                    {{"reason", "input_cap"}});
    Gauge& live =
        reg.gauge("cox_connections_live", "Currently open connections");
    Counter& rejected =
        reg.counter("cox_connections_rejected_total",
                    "Connections shed due to overload (cap or rate limit)");
    Counter& bytes_in =
        reg.counter("cox_connection_bytes_in_total",
                    "Total application bytes received (summed on close)");
    Counter& bytes_out =
        reg.counter("cox_connection_bytes_out_total",
                    "Total application bytes sent (summed on close)");

    // Chain onto the user's connection callback rather than replacing it.
    // The connection callback fires on connect AND disconnect; distinguish via
    // connected(). The disconnect notification is NOT guaranteed exactly-once
    // across teardown paths (handleClose and connectDestroyed can both fire for
    // the same connection during shutdown), so disconnect accounting is made
    // idempotent per connection.
    //
    // Idempotency + bounded memory: a connection NAME is inserted on connect
    // and erased after its disconnect is accounted. The set therefore holds at
    // most the live-connection count plus connections mid-teardown -- it does
    // NOT grow with total lifetime connections. (Names are unique per
    // connection, so connect-insert / disconnect-erase pair up cleanly, and a
    // duplicate disconnect finds the name already erased and is ignored.)
    //
    // In a multi-threaded server the connection callback runs on different io
    // loops, so the set and its mutex are shared via shared_ptr and the set is
    // guarded. (The metric atomics are already thread-safe.)
    struct Dedup {
        std::mutex mu;
        std::set<std::string> live;
    };
    auto dedup = std::make_shared<Dedup>();
    net::ConnectionCallback prev_conn = server.connectionCallback();
    server.setConnectionCallback(
        [prev_conn, dedup, &accepted, &closed_peer, &closed_idle, &closed_write,
         &closed_maxage, &closed_inputcap, &live, &bytes_in,
         &bytes_out](const net::TcpConnectionPtr& conn) {
            bool account_disconnect = false;
            {
                std::lock_guard<std::mutex> lk(dedup->mu);
                if (conn->connected()) {
                    dedup->live.insert(conn->name());
                } else {
                    account_disconnect = dedup->live.erase(conn->name()) == 1;
                }
            }
            if (conn->connected()) {
                accepted.inc();
                live.inc();
            } else if (account_disconnect) {
                // First disconnect for a known-live connection (erase returned
                // 1 once); a duplicate disconnect is ignored. Attribute to the
                // specific defense that fired (or a plain peer close).
                switch (conn->closeReason()) {
                    case net::TcpConnection::CloseReason::kIdleTimeout:
                        closed_idle.inc();
                        break;
                    case net::TcpConnection::CloseReason::kWriteTimeout:
                        closed_write.inc();
                        break;
                    case net::TcpConnection::CloseReason::kMaxAge:
                        closed_maxage.inc();
                        break;
                    case net::TcpConnection::CloseReason::kInputCap:
                        closed_inputcap.inc();
                        break;
                    case net::TcpConnection::CloseReason::kPeer:
                        closed_peer.inc();
                        break;
                }
                live.dec();
                // Fold the connection's final byte totals before it is
                // destroyed -- captures churned connection a live-iteration
                // scrape would miss.
                bytes_in.inc(conn->bytesRead());
                bytes_out.inc(conn->bytesWritten());
            }
            if (prev_conn) {
                prev_conn(conn);
            }
        });

    net::TcpServer::OverloadCallback prev_overload = server.overloadCallback();
    server.setOverloadCallback(
        [prev_overload, &rejected](const net::InetAddress& peer) {
            rejected.inc();
            if (prev_overload) {
                prev_overload(peer);
            }
        });
}

namespace {
// Map an HTTP status code to its class label, keeping metric cardinality
// bounded (4 series instead of one per distinct code).
const char* statusClass(int status) {
    if (status >= 500) {
        return "5xx";
    }
    if (status >= 400) {
        return "4xx";
    }
    if (status >= 300) {
        return "3xx";
    }
    if (status >= 200) {
        return "2xx";
    }
    return "1xx";
}
}  // namespace

void installHttpMetrics(Registry& reg, http::HttpServer& server) {
    // Pre-create the per-class counters so the hot path only does inc(), no
    // registry lookup. (4 classes + 1xx.)
    Counter& c1xx = reg.counter("cox_http_requests_total", "HTTP requests",
                                {{"status_class", "1xx"}});
    Counter& c2xx = reg.counter("cox_http_requests_total", "HTTP requests",
                                {{"status_class", "2xx"}});
    Counter& c3xx = reg.counter("cox_http_requests_total", "HTTP requests",
                                {{"status_class", "3xx"}});
    Counter& c4xx = reg.counter("cox_http_requests_total", "HTTP requests",
                                {{"status_class", "4xx"}});
    Counter& c5xx = reg.counter("cox_http_requests_total", "HTTP requests",
                                {{"status_class", "5xx"}});
    Histogram& dur = reg.histogram("cox_http_request_duration_seconds",
                                   "HTTP handler wall time",
                                   Histogram::defaultLatencyBuckets());

    http::HttpServer::RequestObserver prev = server.requestObserver();
    server.setRequestObserver([prev, &c1xx, &c2xx, &c3xx, &c4xx, &c5xx, &dur](
                                  int status, double secs) {
        switch (statusClass(status)[0]) {
            case '2':
                c2xx.inc();
                break;
            case '3':
                c3xx.inc();
                break;
            case '4':
                c4xx.inc();
                break;
            case '5':
                c5xx.inc();
                break;
            default:
                c1xx.inc();
                break;
        }
        dur.observe(secs);
        if (prev) {
            prev(status, secs);
        }
    });

    // Parser rejections, sampled from the server's own counters (they are
    // bumped at the 400/413 sites, before the observer fires).
    http::HttpServer* sp = &server;
    reg.registerSampled(
        "cox_http_rejected_total",
        "HTTP requests rejected by the parser before reaching the handler",
        MetricPoint::Type::kCounter,
        [sp] { return static_cast<double>(sp->rejectedBadRequest()); },
        {{"reason", "bad_request"}});
    reg.registerSampled(
        "cox_http_rejected_total",
        "HTTP requests rejected by the parser before reaching the handler",
        MetricPoint::Type::kCounter,
        [sp] { return static_cast<double>(sp->rejectedBodyTooLarge()); },
        {{"reason", "body_too_large"}});
}

namespace {
// Append one extra label to a caller-supplied label set.
Labels withLabel(Labels base, std::string key, std::string value) {
    base.emplace_back(std::move(key), std::move(value));
    return base;
}
}  // namespace

void installThreadPoolMetrics(Registry& reg, concurrent::ThreadPool& pool,
                              Labels labels) {
    concurrent::ThreadPool* pp = &pool;
    reg.registerSampled(
        "cox_threadpool_tasks_total", "Thread-pool tasks by outcome",
        MetricPoint::Type::kCounter,
        [pp] { return static_cast<double>(pp->tasksRun()); },
        withLabel(labels, "result", "run"));
    reg.registerSampled(
        "cox_threadpool_tasks_total", "Thread-pool tasks by outcome",
        MetricPoint::Type::kCounter,
        [pp] { return static_cast<double>(pp->tasksShed()); },
        withLabel(labels, "result", "shed"));
    reg.registerSampled(
        "cox_threadpool_queue_depth", "Tasks queued and not yet running",
        MetricPoint::Type::kGauge,
        [pp] { return static_cast<double>(pp->queueSize()); }, labels);
}

void installTlsMetrics(Registry& reg, const tls::TlsContext& ctx,
                       Labels labels) {
    const tls::TlsContext* cp = &ctx;
    reg.registerSampled(
        "cox_tls_handshakes_total", "TLS handshakes by outcome",
        MetricPoint::Type::kCounter,
        [cp] { return static_cast<double>(cp->handshakesOk()); },
        withLabel(labels, "result", "ok"));
    reg.registerSampled(
        "cox_tls_handshakes_total", "TLS handshakes by outcome",
        MetricPoint::Type::kCounter,
        [cp] { return static_cast<double>(cp->handshakesFailed()); },
        withLabel(labels, "result", "failed"));
}

void installDnsMetrics(Registry& reg, const net::Resolver& resolver,
                       Labels labels) {
    const net::Resolver* rp = &resolver;
    reg.registerSampled(
        "cox_dns_lookups_total", "DNS lookups by outcome",
        MetricPoint::Type::kCounter,
        [rp] { return static_cast<double>(rp->lookupsOk()); },
        withLabel(labels, "result", "ok"));
    reg.registerSampled(
        "cox_dns_lookups_total", "DNS lookups by outcome",
        MetricPoint::Type::kCounter,
        [rp] { return static_cast<double>(rp->lookupsFailed()); },
        withLabel(labels, "result", "failed"));
}

void installLoopMetrics(Registry& reg, reactor::EventLoop& loop,
                        Labels labels) {
    // Sampled instruments: read the loop's live counters at scrape time. The
    // loop updates these with relaxed atomics on its own thread; reading them
    // from the scrape thread is safe and cheap.
    reactor::EventLoop* lp = &loop;
    reg.registerSampled(
        "cox_loop_iterations_total", "Event-loop iterations",
        MetricPoint::Type::kCounter,
        [lp] { return static_cast<double>(lp->iterations()); }, labels);
    reg.registerSampled(
        "cox_loop_poll_wait_seconds_total", "Cumulative time blocked in poll()",
        MetricPoint::Type::kCounter,
        [lp] { return static_cast<double>(lp->pollWaitMicros()) / 1e6; },
        labels);
}

}  // namespace metrics
