#pragma once

#include "http_request.h"
#include "http_response.h"
#include "inet_address.h"
#include "resolver.h"
#include "tls_context.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace reactor {
class EventLoop;
}

namespace http {

// Multiplexing HTTP/2 client for ONE origin over TLS (ALPN "h2"). Only
// compiled when COX_WITH_HTTP2 is ON.
//
// send() may be called any number of times without waiting; requests run
// concurrently as streams and each callback fires independently, in
// completion order.
//
// Connections (a small pool per origin):
//   * Requests go to the least-loaded connection that can open a stream now.
//   * A new connection is opened only when every existing one is at the
//     server's MAX_CONCURRENT_STREAMS, up to `max_connections`; beyond that,
//     requests queue on the least-loaded connection.
//
// Origin: a numeric address, or a hostname resolved through a net::Resolver
// on first use (requests wait for it). Each connection tries the resolved
// addresses in order (IPv6/IPv4 failover); if no connection can be
// established, the name is resolved again for the next request.
//
// Connection lifecycle is handled for the caller:
//   * When the server sends GOAWAY, or a connection's stream IDs run out
//     (they only increase and cannot be reset), that connection stops taking
//     requests and finishes its in-flight ones; new requests go elsewhere.
//     Requests the server did not process (above GOAWAY's last_stream_id,
//     refused with REFUSED_STREAM, or still queued) are re-sent
//     automatically, at most `max_retries` times.
//   * A request that was in flight when the connection died may have been
//     processed, so it is NOT re-sent; its callback gets the error.
//   * If the server does not agree to h2 in ALPN, requests fail with an
//     error (use HttpClient for HTTP/1.1).
//
// The TlsContext must be a client context; Http2Client sets ALPN to {"h2"}
// on it. All callbacks run on `loop`. Use the client from that loop's thread.
class Http2Client {
public:
    using ResponseCallback =
        std::function<void(const HttpResponse& resp, const std::string& error)>;

    struct Options {
        // Re-sends of a request the server provably did not process.
        int max_retries = 3;
        // Connections opened in parallel to this origin (>= 1). See the
        // class comment for when a new one is opened.
        size_t max_connections = 4;
        // Requests queued per connection beyond the stream limit.
        size_t max_queued = 1000;
        // Response body cap (0 = unlimited).
        size_t max_response_body = 64 * 1024 * 1024;
        // Per-stream download window advertised to the server.
        uint32_t initial_window_size = 65535;
        // Highest stream ID used per connection before moving to a new one
        // (0 = protocol maximum, 2^31-1). Mostly for tests.
        uint32_t max_stream_id = 0;
        // Time allowed to establish a connection (TCP + TLS + h2 SETTINGS
        // ready). Requests waiting for it then fail. The underlying connector
        // otherwise retries an unreachable server forever.
        uint32_t connect_timeout_ms = 10000;
    };

    // Numeric origin. `host_header` is :authority and SNI; defaults to the
    // server's IP.
    Http2Client(reactor::EventLoop* loop, const net::InetAddress& server,
                tls::TlsContextPtr tls_ctx, std::string host_header = "");
    Http2Client(reactor::EventLoop* loop, const net::InetAddress& server,
                tls::TlsContextPtr tls_ctx, std::string host_header,
                Options options);
    // Hostname origin, resolved through `resolver` (which must outlive the
    // client). :authority and SNI default to `host`.
    Http2Client(reactor::EventLoop* loop, const std::string& host,
                uint16_t port, net::Resolver* resolver,
                tls::TlsContextPtr tls_ctx, std::string host_header = "");
    Http2Client(reactor::EventLoop* loop, const std::string& host,
                uint16_t port, net::Resolver* resolver,
                tls::TlsContextPtr tls_ctx, std::string host_header,
                Options options);
    ~Http2Client();  // pending callbacks get "client destroyed"

    Http2Client(const Http2Client&) = delete;
    Http2Client& operator=(const Http2Client&) = delete;

    void send(const HttpRequest& req, ResponseCallback cb);
    void get(const std::string& path, ResponseCallback cb);

    // Gracefully close: GOAWAY to the server; in-flight requests finish,
    // new ones fail.
    void shutdown();

    // Number of TCP connections opened so far (tests / diagnostics).
    uint64_t connectionsOpened() const;
    // Connections currently open or connecting (including draining ones).
    size_t openConnections() const;
    // Requests re-sent on a new connection after GOAWAY / refusal.
    uint64_t retries() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace http
