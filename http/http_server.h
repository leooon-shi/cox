#pragma once

#include "http_context.h"
#include "http_request.h"
#include "http_response.h"
#include "inet_address.h"
#include "server_stream.h"
#include "tcp_server.h"
#include "tls_context.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace reactor {
class EventLoop;
}

namespace http {
// HttpServer is an HTTP/1.1 server built on net::TcpServer, optionally
// TLS-terminating (HTTPS). It gives each connection an HttpContext parser,
// dispatches each fully-parsed request to a user handler, serializes the
// response, and manages keep-alive.
//
// For HTTPS, construct with a server-role TlsContext; each accepted connection
// is wrapped in TLS and the handshake completes before requests are parsed.
class HttpServer {
public:
    using HttpHandler =
        std::function<void(const HttpRequest& req, HttpResponse* resp)>;
    // Plaintext HTTP server.
    HttpServer(reactor::EventLoop* loop, const net::InetAddress& addr,
               std::string name);
    // HTTPS server: tls_ctx is a server-role TlsContext (cert/key loaded).
    HttpServer(reactor::EventLoop* loop, const net::InetAddress& addr,
               std::string name, tls::TlsContextPtr tls_ctx);
    ~HttpServer();

    void setHttpHandler(HttpHandler handler) { handler_ = std::move(handler); }
    // Async / streaming handler (see server_stream.h): the handler gets a
    // ServerStream and may answer after returning, and stream the response
    // body. Takes precedence over setHttpHandler. Currently served on
    // HTTP/2 connections; on HTTP/1.1 connections the server runs it and
    // waits for the response before reading the next request (phase 2 of
    // docs/insights/features/http-streaming-design.md adds chunked
    // streaming there).
    using StreamHandler = std::function<void(const ServerStreamPtr&)>;
    void setStreamHandler(StreamHandler handler) {
        stream_handler_ = std::move(handler);
    }
    void setThreadNum(int n) { server_.setThreadNum(n); }

    using ConnectionObserver =
        std::function<void(const net::TcpConnectionPtr&)>;
    void setConnectionObserver(ConnectionObserver obs) {
        connection_observer_ = std::move(obs);
    }

    // Per-request observer, invoked AFTER each request is handled (or rejected)
    // with the response status code and the time spent in the handler. This is
    // the hook metrics/observability attaches to -- the HTTP layer owns it and
    // has no dependency on the metrics module. `status` is the HTTP status
    // sent (e.g. 200, 404, 400, 413); `handler_seconds` is wall time in the
    // handler (0 for parser-rejected requests that never reached it).
    using RequestObserver =
        std::function<void(int status, double handler_seconds)>;
    void setRequestObserver(RequestObserver obs) {
        request_observer_ = std::move(obs);
    }
    // Read the current request observer (to chain onto it). Returns a copy.
    RequestObserver requestObserver() const { return request_observer_; }

    void setKeepAliveTimeout(uint32_t seconds) {
        server_.setIdleTimeout(seconds);
    }

    // Maximum request body size in bytes (0 = unlimited). A real per-request
    // limit enforced by the parser: a Content-Length above it is rejected up
    // front with 413, and a chunked body is rejected once it exceeds the cap.
    // Defaults to HttpContext::kDefaultMaxBodyBytes.
    void setMaxBodySize(size_t bytes) { max_body_bytes_ = bytes; }

    // Maximum unconsumed bytes buffered per connection at the TCP layer before
    // the connection is force-closed (coarse memory bound, distinct from the
    // per-request body limit above).
    void setMaxInputBufferSize(size_t bytes) {
        server_.setMaxInputBufferSize(bytes);
    }

    // HTTP/2 (only with COX_WITH_HTTP2). An HTTPS server whose TlsContext
    // advertises "h2" via setAlpnProtocols({"h2", "http/1.1"}) serves h2 to
    // clients that negotiate it and HTTP/1.1 to the rest, with the same
    // handler. Requests arriving over h2 report version() == "HTTP/2".
    struct Http2Options {
        // SETTINGS_MAX_CONCURRENT_STREAMS. Excess streams are refused
        // (REFUSED_STREAM, safe for the client to retry).
        uint32_t max_concurrent_streams = 100;
        // SETTINGS_MAX_HEADER_LIST_SIZE (bytes; larger lists get 431).
        uint32_t max_header_list_bytes = 64 * 1024;
        // SETTINGS_INITIAL_WINDOW_SIZE: per-stream request-body window.
        uint32_t initial_window_size = 65535;
        // Connection-level receive window; 0 keeps the RFC default (65535).
        uint32_t connection_window_size = 0;
        // SETTINGS_MAX_FRAME_SIZE (16384..16777215).
        uint32_t max_frame_size = 16384;
        // Highest client stream ID served per connection (odd IDs only
        // increase; protocol max 2^31-1). When reached, the server sends a
        // graceful GOAWAY and the client continues on a new connection.
        // 0 = protocol maximum.
        uint32_t max_stream_id = 0;
        // Requests per connection before a graceful GOAWAY (nginx
        // keepalive_requests). 0 = unlimited.
        uint32_t max_requests_per_connection = 0;
        // Delay between the GOAWAY shutdown notice and the final GOAWAY, so
        // requests already in flight are accepted (about one RTT).
        uint32_t goaway_notice_ms = 1000;
        // After the final GOAWAY, how long remaining streams may run before
        // the connection is force-closed. 0 = wait indefinitely (server
        // drain still applies its own deadline).
        uint32_t goaway_grace_seconds = 30;
    };
    // Set the HTTP/2 options. Throws std::invalid_argument on values outside
    // RFC 9113 bounds. Call before start().
    void setHttp2Options(const Http2Options& opts);
    const Http2Options& http2Options() const { return h2_opts_; }
    // Shorthand for the most common knob.
    void setHttp2MaxConcurrentStreams(uint32_t n) {
        h2_opts_.max_concurrent_streams = n;
    }
    // Begin a graceful shutdown of every HTTP/2 connection (GOAWAY, finish
    // in-flight streams, then close). HTTP/1.1 connections are untouched.
    // TcpServer::shutdown() (tcpServer().shutdown(...)) does this
    // automatically for h2 connections. Must run on the loop thread.
    void goAwayHttp2Connections();
    // HTTP/2 connections currently open (for tests/metrics).
    size_t http2Connections() const;
    // All connections the server is tracking (HTTP/1.1 and h2, plain and
    // TLS). Loop thread only; for tests and diagnostics.
    size_t trackedConnections() const { return conns_.size(); }
    // True if this build includes HTTP/2 support.
    static constexpr bool http2Supported() {
#ifdef COX_WITH_HTTP2
        return true;
#else
        return false;
#endif
    }

    net::TcpServer& tcpServer() { return server_; }
    bool usesTls() const { return tls_ctx_ != nullptr; }

    // Requests the parser rejected before they reached the handler, by reason
    // (lifetime counts, relaxed atomics, safe to read from any thread).
    // bad_request -> 400 (malformed), body_too_large -> 413.
    uint64_t rejectedBadRequest() const {
        return rejected_bad_request_.load(std::memory_order_relaxed);
    }
    uint64_t rejectedBodyTooLarge() const {
        return rejected_too_large_.load(std::memory_order_relaxed);
    }

    void start();

private:
    void onConnection(const net::TcpConnectionPtr& conn);
    // HTTP/1.1, phase 1: run the stream handler and collect a response it
    // completed before returning (500 otherwise).
    HttpResponse runStreamHandlerSync(const HttpRequest& req, bool keep_alive);
    void onPlainMessage(const net::TcpConnectionPtr& conn, buffer::Buffer* buf);

    // Parse from `in`, dispatch complete requests, write responses via `send`,
    // and half-close via `close` when a request is not keep-alive.
    void processInput(const std::string& conn_name, buffer::Buffer* in,
                      const std::function<void(const std::string&)>& send,
                      const std::function<void()>& close);
    tls::TlsContextPtr tls_ctx_;  // null for plaintext
    HttpHandler handler_;
    StreamHandler stream_handler_;
    ConnectionObserver connection_observer_;
    RequestObserver request_observer_;
    size_t max_body_bytes_{HttpContext::kDefaultMaxBodyBytes};
    Http2Options h2_opts_;
    std::atomic<uint64_t> rejected_bad_request_{0};
    std::atomic<uint64_t> rejected_too_large_{0};

    // Per-connection state, keyed by connection name (accessed on the loop
    // thread). For TLS connections, holds the TlsConnection and a plaintext
    // reassembly buffer; for both, the HTTP parse context.
    struct PerConn;
    std::map<std::string, std::shared_ptr<PerConn>> conns_;
#ifdef COX_WITH_HTTP2
    // Switch a TLS connection to HTTP/2 after ALPN selected "h2".
    void startHttp2(PerConn& pc);
    // Run the two-step GOAWAY on one h2 connection and close it when done.
    void goAwayHttp2(const std::string& name, bool immediate);
    void maybeCloseHttp2(PerConn& pc);
#endif

    // Declared LAST so it is destroyed FIRST: ~TcpServer fires connectDestroyed
    // on lingering connections, which calls back into onConnection() and
    // touches conns_. Tearing the server down before conns_ ensures those
    // callbacks see a valid map.
    net::TcpServer server_;
};
}  // namespace http
