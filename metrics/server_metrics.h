#pragma once

#include "registry.h"

namespace net {
class TcpServer;
}
namespace http {
class HttpServer;
}
namespace reactor {
class EventLoop;
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
//   cox_connection_rejected_total   (counter)  -- overload / cap / rate-limit
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
// Call AFTER the application has set its own observer (if any), before start().
void installHttpMetrics(Registry& reg, http::HttpServer& server);

// Built-in EventLoop instrumentation via the loop's lightweight stat accessors
// (reactor has no dependency on this module). Registers SAMPLED instruments
// read at scrape time:
//   cox_loop_iterations_total     (counter; loop.iterations())
//   cox_loop_poll_wait_seconds_total (counter; loop.pollWaitMicros/1e6)
// `labels` lets a multi-loop server distinguish loops (e.g. {{"loop", "io0"}}).
void installLoopMetrics(Registry& reg, reactor::EventLoop& loop,
                        Labels labels = {});

}  // namespace metrics
