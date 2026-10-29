# Cox User Guide

Cox is an event-driven networking library for Linux, written in C++20. It
provides a reactor event loop, TCP and Unix-domain sockets, TLS, timers, a
worker thread pool, flow/overload control, and production hardening (timeouts,
backpressure, graceful shutdown). This guide shows how to use it.

For the design rationale behind each feature, see `insights/features/`.

## Contents
1. Requirements and build
2. Core concepts
3. Your first echo server
4. Writing a client
5. Multi-threading
6. TLS and mutual TLS
7. Kubernetes certificate rotation
8. HTTP, HTTPS, and REST
9. Connecting by hostname (DNS)
10. Unix-domain sockets
11. Timers
12. Offloading blocking work
13. Flow control and overload protection
14. Production hardening knobs
15. Graceful shutdown
16. Logging
17. Gotchas and rules

---

## 1. Requirements and build

Requirements:
- Linux (epoll, eventfd, timerfd, inotify)
- clang with libc++, CMake 3.20+, OpenSSL 3
- GoogleTest (bundled as a submodule)

Build with the provided script:

```
./build.sh  --release                  # library only
./build.sh. --debug --build-tests      # with tests
./build.sh  --release --build-examples
```

All output goes to `build/` (binaries in `build/bin/`).

Link your application against the module libraries you use, for example
`cow_net` (which pulls in the reactor, buffer, flowcontrol, logging, utilities)
and `cow_tls` for TLS.

## 2. Core concepts

- **EventLoop** - one per thread; runs the poll/dispatch cycle. Everything is
  driven by a loop.
- **Callbacks run on the loop thread.** Never block them. Offload blocking work
  to a thread pool.
- **Thread-safety** - to touch loop-owned state from another thread, use
  `loop.runInLoop(task)`. `TcpConnection::send()` is already thread-safe.
- **Connections are `shared_ptr`** (`TcpConnectionPtr`); the library keeps them
  alive across callbacks.

## 3. Your first echo server

```cpp
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_server.h"
#include "buffer.h"

using namespace net;

int main()
{
    reactor::EventLoop loop;
    InetAddress addr{9000};                     // listen on 0.0.0.0:9000
    TcpServer server(&loop, addr, "echo");

    server.setConnectionCallback([](const TcpConnectionPtr& conn) {
        // Fires on connect and disconnect.
        // conn->connected() distinguishes the two.
    });

    server.setMessageCallback([](const TcpConnectionPtr& conn,
                                 buffer::Buffer* buf) {
        conn->send(buf->retrieveAllAsString());   // echo back
    });

    server.start();
    loop.loop();
}
```

The message callback receives a `Buffer`; consume bytes with `retrieve*`. Send
with `conn->send(...)`.

## 4. Writing a client

```cpp
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_client.h"

using namespace net;

reactor::EventLoop loop;
InetAddress server_addr("127.0.0.1", 9000);
TcpClient client(&loop, server_addr, "client");
client.enableRetry();                         // auto-reconnect with backoff

client.setConnectionCallback([](const TcpConnectionPtr& conn) {
    if (conn->connected()) conn->send("hello");
});

client.setMessageCallback([](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
    auto reply = buf->retrieveAllAsString();
    // ... use reply ...
});

client.connect();
loop.loop();
```

## 5. Multi-threading

By default all connections run on the loop you pass to `TcpServer`. To spread
connections across CPU cores, set an io-thread pool:

```cpp
TcpServer server(&loop, addr, "svc");
server.setThreadNum(4);         // 4 io loops; connections round-robined
server.start();
```

Each connection is pinned to one io loop; its callbacks always run on that
loop's thread, so per-connection state needs no locks.

## 6. TLS and mutual TLS

```cpp
#include "tls_context.h"
#include "tls_connection.h"

using namespace tls;

auto ctx = std::make_shared<TlsContext>(Role::kServer);
ctx->useCertificateAndKey("server.crt", "server.key");
ctx->loadCaFile("ca.crt");
ctx->setVerifyPeer(true);       // require + verify client cert (mutual TLS)

server.setConnectionCallback([ctx, &connections](const TcpConnectionPtr& conn) {
    if (conn->connected())
    {
        auto tls_conn = std::make_shared<TlsConnection>(conn, ctx, Role::kServer);
        tls_conn->setMessageCallback([](const TlsConnectionPtr& t,
                                        std::string& plaintext) {
            t->send(plaintext);             // decrypted in, encrypted out
        });
        connections[conn->name()] = tls_conn;     // keep it alive
        tls_conn->start();                          // begin handshake
    }
    else
    {
        connections.erase(conn->name());
    }
});
```

The client side is symmetric with `Role::kClient` and a handshake-done callback
that sends once the channel is secure. On a client, pass the server hostname so
Cox sends SNI **and** verifies the certificate actually belongs to that name:

```cpp
auto tls_conn = std::make_shared<TlsConnection>(conn, client_ctx,
                                                Role::kClient,
                                                "api.example.com");
```

Without a hostname, verification only proves the certificate was signed by a
trusted CA - not that it belongs to the host you meant to reach. The HTTPS client
and connection pools (section 8) do this for you.

Important: hold the `TlsConnection` alive for the underlying connection's
lifetime (store it, e.g. in a map keyed by `conn->name()`).

### Serving several hostnames on one port (SNI)

Register an extra certificate per hostname; the context you call this on stays
the default for unknown names:

```cpp
auto api_ctx = std::make_shared<TlsContext>(Role::kServer);
api_ctx->useCertificateAndKey("api.crt", "api.key");

ctx->addSniContext("api.example.com", api_ctx);
ctx->addSniContext("*.internal.example.com", internal_ctx);  // one label
```

A client asking for an unregistered name gets the default certificate rather
than a handshake failure. Server-side you can see what was requested with
`tls_conn->requestedServerName()` - but treat it as untrusted input, since the
client chooses it.

### OCSP stapling

Only useful for publicly-trusted, CA-issued certificates served to public
clients - skip it for private-CA, self-singed, or short-lived certs.

```cpp
#include "ocsp_stapler.h"

tls::Ocspstapler stapler("server.crt", "issuer.crt");           // URL from cert's AIA
stapler.refresh();                                              // BLOCKING http fetch -- worker thread or timer
stapler.attachTo(*ctx);                                         // handshakes now serve the cached response
```

If an external agent (certbot, a sidecar) already writes the response, load it
instead of fetching:

```cpp
stapler.loadFromFile("/var/run/ocsp/resp.der");
```

Refresh is not automatic. Schedule it with a timer, well before expiry:

```cpp
loop.runEvery(3600*3600, [&stapler, &pool] {
    int64_t left = stapler.secondsUntilExpiry();    // -1 = no expiry hint
    if (left >= 0 && left < 7200)
    {
        pool.run([&stapler] {stapler.refresh(); })  // off the loop thread.
    }
});
```

If no valid response is cached the handshake simply proceeds without a staple,
so a responder outage degrades stapling rather than breaking your server.

## 7. Kubernetes certificate rotation

Mount a TLS Secret so the pod sees `tls.crt`, `tls.key`, `ca.crt`, then reload
on rotation without restarting:

```cpp
#include "cert_reloader.h"

auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kServer);
ctx->useCertificateAndKey("/etc/tls/tls.crt", "/etc/tls/tls.key");
ctx->loadCaFile("/etc/tls/ca.crt");
ctx->setVerifyPeer(true);

tls::CertReloader reloader (&loop, ctx);
reloader.startWatching("/etc/tls");                 // inotify; handles the k8s atomic swap
// or: reloader.startPolling(cert, key, ca, 30000);         // poll every 30s
```

New connections use the fresh certificate after rotation; existing connections
keep their session. A bad/partial update is rejected and the previous
certificate is retained.

## 8. HTTP, HTTPS and REST

### A REST server

```cpp
#include "event_loop.h"
#include "http_server.h"
#include "router.h"

reactor::EventLoop loop;
http::HttpServer server(&loop, net::InetAddress(8080), "rest");

http::Router router;
router.get("/health", [](const http::HttpRequest&, const http::PathParams&,
                         http::HttpResponse* resp) {
    resp->setStatus(200);
    resp->setContentType("application/json");
    resp->setBody(R"({"status":"ok"})");
});

// {id} captures one path segment (percent-decoded).
router.get("/users/{id}", [](const http::HttpRequest&,
                             const http::PathParams& p,
                             http::HttpResponse* resp) {
    resp->setStatus(200);
    resp->setBody("user " + p.at("id"));
});

server.setHttpHandler([&router](const http::HttpRequest& req,
                                http::HttpResponse* resp){
    router.route(req, resp);    // 404 if no path matches, 405 on wrong method   
});

server.setThreadNum(4);         // Optional: spread connections over io loops
server.start();
loop.loop();
```

Keep-alive, pipelining, chunked bodies, and the TCP hardening knobs (section 14)
all apply automatically.

### Query strings and form bodies

```cpp
// GET /search?q=hello+world&page=2
std::string q       = req.queryParam("q");              // "hello world" (decoded)
std::string page    = req.queryParam("page")            // "2"
bool has_page       = req.hasQueryParam("page");
const auto& all     = req.queryParams();                // whole map

// POST with Content-Type: application/x-www-form-urlencoded
auto form = req.forParams();                              // decoded key/value pairs

// Standalone helpers
std::string decoded = http::urlDecode("a%20b");
std::string encoded = http::urlEncode("a b");
```

Note `+` decodes to a space in queries and form bodies, but **not** in path
segments - so a path like `/tags/c++` arrives intact.

### HTTPS server

Pass a server-role `TlsContext` (see section 6); each connection completes its
handshake before any request is parsed:

```cpp
http::HttpServer server(&loop, net::InetAddress(8443), "api", server_ctx);
```

### Making requests

```cpp
#include "http_client.h"

http::HttpClient client(&loop, net::InetAddress("127.0.0.1", 8080));
client.get("/health", [](const http::HttpResponse& resp,
                         const std::string& err) {
    if (!err.empty()) return;               // transport/parse failure
    resp.statusCode(); resp.body();
});
```

`HttpClient` reuses one keep-alive connection and is **sequential** - issue the
next request from the previous callback. For HTTPS, pass a client `TlsContext`.

### Connection pooling

For concurrency, or to amortize TLS handshakes across many requests:

```cpp
#include "connection_pool.h"
#include "multi_origin_pool.h"

http:ConnectionPool pool(&loop, origin_addr);           // optionally + TlsContext
pool.setMaxConnections(8);                              // then queue beyond the cap
pool.setIdleTimeout(60);                                // evict idle connections
pool.request(req, callback);

// Several origins behind one object (Go http.Transport style):
http.MultiOriginPool multi(&loop);
multi.setTlsContext("api.example.com", 443, client_ctx);
multi.request("api.example.com", 443, req, callback);
```

Pools are single-loop: create one per loop and do not share them across threads.

## 9. Connecting by hostname (DNS)

Clients and pools accept a hostname plus a `Resolver` instead of a numeric
address. Resolution is asynchronous and never blocks the loop; requests issued
while it is in flight are queued and dispatched once the name resolves.

```cpp
#include "resolver.h"
#include "thread_pool.h"

concurrent::ThreadPool dns_pool("dns");
dns_pool.start(2);
net::ThreadPoolResolver resolver(&loop, &dns_pool);         // getaddrinfo, offload
resolver.setCacheTtl(60);                                   // positive-cache TTL

http::HttpClient client(&loop, "api.example.com", 443, &resolver, client_ctx);
client.get("/v1/things", callback);

// Pools take the same form:
http::ConnectionPool pool(&loop, "api.example.com", 443, &resolver);
http::MultiOriginPool multi(&loop);
multi.setResolver(&resolver);               // now accepts hostname
```

The `Host` header defaults to the hostname, not the resolved IP. If resolution
fails, every queued request completes with a `"DNS resolution failed: ..."`
error and a later request retries.

Dual-stack: when a name resolves to several addresses (e.g. an `AAAA` and an
`A` record, as in a dual-stack Kubernetes cluster), the full ordered list is
kepts and the connector tries each address in turn, failing over immediately to
the next if one is unreachable. A client therefore connects as long as *any*
resolved address accepts the connection, regardless of which family the OS
orders first.

If Cox was built with c-ares available, `net::CaresResolver` is an alternative
backend that uses no worker threads at all:

```cpp
#ifdef COX_HAVE_CARES
net::CaresResolver resolver(&loop);
#endif
```

Use `ThreadPoolResolver` (the default) when host name configuration such as
`/etc/hosts` must be honored; see `insights/features/dns-resolution.md` for the
tradeoff.

## 10. Unix-domain sockets

The same server/client code works over a filesystem socket -- efficient IPC for
processes on the same host or container:

```cpp
InetAddress addr = InetAddress::unixDomain("/run/app.sock");
TcpServer server(&loop, addr, "ipc");
// ... identical callbacks ...
```

## 11. Timers

```cpp
loop.runAfter(5000, []{ /* once, in 5 seconds */ });
auto id = loop.runEvery(1000, []{ /* every second */ });
loop.cancel(id);   // cancel from the loop thread
```

## 12. Offloading blocking work

Never block a loop callback. Offload to a worker pool and post the result back:

```cpp
#include "thread_pool.h"

concurrent::ThreadPool pool("work");
pool.setMaxQueueSize(1000);
pool.start(8);

server.setMessageCallback([&pool](const TcpConnectionPtr& conn,
                                  buffer::Buffer* buf) {
    auto req = buf->retrieveAllAsString();
    pool.run([conn, req] {
        auto resp = blockingWork(req);                  // off the loop
        conn->getLoop()->runInLoop([conn, resp] {
            conn->send(resp);                           // back on the loop
        });
    });
});
```

Use `pool.tryRun(...)` instead of `run(...)` to shed load when the queue is
full.

## 13. Flow control and overload protection

Rate-limit new connections and shed early:

```cpp
server.setAcceptRateLimit(1000, 200);   // 1000 conn/s, burst 200
server.setOverloadCallback([](const InetAddress& peer) {
    // record a metric; the connection was already shed
});
```

Bound concurrent in-flight work in a handler:

```cpp
flowcontrol::ConcurrencyLimiter limiter(500);

server.setMessageCallback([&limiter](const TcpConnectionPtr& conn,
                                     buffer::Buffer* buf) {
    if (auto permit = limiter.tryAcquire())
    {
        handle(conn, buf);           // permit releases at scope end
    }
    else
    {
        conn->send(busyResponse());   // shed with a cheap reply
    }
});
```

Standalone primitives are also available: `flowcontrol::TokenBucket` (rate) and
`flowcontrol::Backoff` (per-key jittered retry backoff).

## 14. Production hardening knobs

Apply to every connection via the server:

```cpp
server.setIdleTimeout(60);                      // Close silent connection after 60s
server.setWriteTimeout(30);                     // Close slow consumers that can't drain
server.setMaxInputBufferSize(1<<20);            // Bound read-safe memory (1 MB)
server.setMaxConnectionAge(3600);               // Recycle after 1 hour
server.setMaxConnections(10000);                // Cap concurrent connections
```

Egress backpressure (per connection):

```cpp
conn->setHighWaterMarkCallback([](const TcpConnectionPtr& c, size_t queued) {
    // stop producing until drained
}, 64 << 20 );
```

Ingress backpressure (per connection): `conn->stopRead()` / `conn->startRead()`.

SIGPIPE is ignored automatically; a write to a closed peer surfaces as a normal
error, not a process kill.

## 15. Graceful shutdown

```cpp
server.shutdown(30, [&loop] {       // 30s drain deadline
    loop.quit();                    // exit once fully drained
});
```

Drain stops accepting, half-closes connections so peers see EOF, waits for them
to finish, force-closes stragglers at the deadline, then invokes the completion
callback. Wire this to SIGPIPE/SIGINT via an async-signal-safe flag polled by a
timer (see `examples/echo/echo_server.cc`).

## 16. Logging

Levels are `debug/info/warning/error/fatal`, filtered at `Info` by default:

```cpp
#include "logger.h"

logging::Logger log;                        // Info, human-readable, stdout
log.info("server started");
log.setLevel(logging::LogLevel::Debug);     // enable Debug
```

Attach structured fields instead of interpolating context info the message -
they survive into JSON as real fields you can filter on:

```cpp
log.info("request done", {{"method", "GET"}, {"path", "/x"}, {"status", "200"}});
```

### JSON output and destinations

A **Formatter** decides how a line looks; a **Sink** decides where it goes. They
compose, so JSON-to-a-rotating-file needs no special class:

```cpp
#include "sink.h"

auto rot = logging::Sink::newRotatingFileSink("/var/log/svc.log",
                                              64 << 20,   // roll at 64 MB
                                              5);         // keep 5 old files
logging::Logger jlog(logging::LogLevel::Info,
                    logging::Formatter::newJsonFormatter(),
                    std::shared_ptr<logging::Sink>(std::move(rot)));
```

Other sinks: `newStdoutSink()`, `newFileSink(path)`, and `newMultiSink(...)` to
fan out to several at once. Disk use is bounded by roughly
`max_bytes x (max_files + 1)` - size it deliberately.

### Never do file I/O on an event-loop thread

A synchronous write stalls every connection on that loop. Wrap the sink in an
`AsyncSink`, which hands lines to a background writer:

```cpp
#include "async_sink.h"

auto sink = std::make_shared<logging::AsyncSink>(
    logging::Sink::newRotatingFileSink("/var/log/svc.log", 64 << 20, 5));
log.setSink(sink);
```

Its buffer is **bounded**: under sustained overload it drops lines rather than
blocking your loop or growing until OOM. Drops are counted, not hidden - check
`sink->droppedCount()` and watch for the `"dropped N log line(s)"` summary. If
you see drops, the destination is too slow or the buffer too small.

`Fatal` records flush immediately so a crash explanation is never stuck in a
buffer; lower the threshold with `log.setFlushLevel(logging::LogLevel::Error)`.

## 17. Gotchas and rules

- Do not block a loop callback. Offload to a thread pool (section 12).
- Touch loop-owned state only on that loop's thread; otherwise use
  `runInLoop`. `send()` is the thread-safe exception.
- Keep `TlsConnection` objects alive for the connection's lifetime.
- Create and run an `EventLoop` on the same thread.
- For Kubernetes cert rotation, watch the mount directory, not the file.
- Signal handlers must only set a flag; do real work on the loop thread.
- set `setThreadNum`, rate limits, and hardening knobs before `start()`.
- Logging to a file from a loop thread needs an `AsyncSink` (section 16); a
  synchronous write stalls every connection on that loop.
- On a TLS client, pass the server hostname so the certificate is checked
  against it - chain verification alone proves only "signed by a trusted CA".
- `OcspStapler::refresh()` blocks; drive it from a timer plus a worker thread,
  never directly on a loop.

## Further reading
- `insights/features` - per-feature design and technology-choice rationale.
- `insights/roadmap.md` - status, deferred items, and discarded directions.
- `examples/echo/` - runnable echo server and client.
- `examples/http/rest_server.cc` - runnable REST server (health, path params,
  query strings, form bodies).
- `examples/benchmark/` -- throughput / latency benchmarks; see
  `insights/features/benchmarks.md`.