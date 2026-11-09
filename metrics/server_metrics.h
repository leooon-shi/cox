#pragma once

#include "registry.h"

namespace net {
class TcpServer;
class Resolver;
}  // namespace net
namespace http {
class HttpServer;
}
namespace reactor {
class EventLoop;
}
namespace concurrent {
class ThreadPool;
}
namespace tls {
class TlsContext;
}

namespace metrics {
// Built-in instrumentation: wire a TcpServer's lifecycle to a set of standard
// Cox metrics, using only the server's PUBLIC hooks so the core net library has
// no dependency on this module (the dependency points metrics -> net, never the
// reverse).
//
// OPT-IN: a plain server emits nothing until installServerMetrics() is called,
// so an application that does not want metrics pays zero cost.
//
// Composition: the helper CHAINS onto any callback already set on the server
// (it does not clobber a user's connection/overload callback). Call it AFTER
// the application has set its own callbacks, and BEFORE start().
//
// Metrics registered (on `reg`):
//   cox_connections_accepted_total  (counter)
//   cox_connections_closed_total    (counter)
//   cox_connections_live            (gauge)
//   cox_connections_rejected_total  (counter)  -- overload / cap / rate-limit
//   cox_connection_bytes_in_total   (counter)  -- summed from each connection
//   cox_connection_bytes_out_total  (counter)     on close (captures churn)
//
// Bytes are accumulated when a connection CLOSES (its final bytesRead/Written
// are added to the server totals), so a long-lived connection's in-flight
// bytes appear only after it closes -- correct for a monotonic _total.
void installServerMetrics(Registry& reg, net::TcpServer& server);

// Built-in HTTP instrumentation via HttpServer's public RequestObserver hook
// (cox_http has no dependency on this module). OPT-IN; chains onto any existing
// request observer. Registers:
//   cox_http_requests_total{status_class}  (counter; class = 2xx/3xx/4xx/5xx)
//   cox_http_request_duration_seconds      (histogram; handler wall time)
//   cox_http_rejected_total{reason}        (counter, sampled; requests the
//       parser rejected before the handler: reason = bad_request (400) or
//       body_too_large (413). These are ALSO counted in requests_total{4xx}.)
// Call AFTER the application has set its own observer (if any), before start().
// The server must outlive the registry's scrapes.
void installHttpMetrics(Registry& reg, http::HttpServer& server);

// Built-in EventLoop instrumentation via the loop's lightweight stat accessors
// (reactor has no dependency on this module). Registers SAMPLED instruments
// read at scrape time:
//   cox_loop_iterations_total     (counter; loop.iterations())
//   cox_loop_poll_wait_seconds_total (counter; loop.pollWaitMicros()/1e6)
// `labels` lets a multi-loop server distinguish loops (e.g. {{"loop","io0"}}).
void installLoopMetrics(Registry& reg, reactor::EventLoop& loop,
                        Labels labels = {});

// Built-in ThreadPool instrumentation (sampled at scrape time from the pool's
// relaxed counters; concurrent has no dependency on this module). Registers:
//   cox_threadpool_tasks_total{result="run"|"shed"}  (counter)
//   cox_threadpool_queue_depth                       (gauge; queued tasks)
// `labels` distinguishes pools (e.g. {{"pool","dns"}}). The pool must outlive
// the registry's scrapes.
void installThreadPoolMetrics(Registry& reg, concurrent::ThreadPool& pool,
                              Labels labels = {});

// Built-in TLS instrumentation from a TlsContext's handshake counters (every
// TlsConnection built on the context records its outcome). Registers:
//   cox_tls_handshakes_total{result="ok"|"failed"}  (counter)
// "failed" includes a peer closing before the handshake completed. `labels`
// distinguishes contexts (e.g. a server vs a client context). The context
// must outlive the registry's scrapes.
void installTlsMetrics(Registry& reg, const tls::TlsContext& ctx,
                       Labels labels = {});

// Built-in DNS instrumentation from a Resolver's lookup counters (works for
// either backend). Registers:
//   cox_dns_lookups_total{result="ok"|"failed"}  (counter)
// A cache hit counts as ok. The resolver must outlive the registry's scrapes.
void installDnsMetrics(Registry& reg, const net::Resolver& resolver,
                       Labels labels = {});

}  // namespace metrics
