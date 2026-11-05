#pragma once

#include "http_context.h"
#include "http_request.h"
#include "http_response.h"
#include "inet_address.h"
#include "tcp_server.h"
#include "tls_context.h"

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
    void setMaxBodySize(size_t bytes) { max_body_size_ = bytes; }

    // Maximum unconsumed bytes buffered per connection at the TCP layer before
    // the connection is force-closed (coarse memory bound, distinct from the
    // per-request body limit above).
    void setMaxInputBufferSize(size_t bytes) {
        server_.setMaxInputBufferSize(bytes);
    }

    net::TcpServer& tcpServer() { return server_; }
    bool usesTls() const { return tls_ctx_ != nullptr; }

    void start();

private:
    void onConnection(const net::TcpConnectionPtr& conn);
    void onPlainMessage(const net::TcpConnectionPtr& conn, buffer::Buffer* buf);

    // Parse from `in`, dispatch complete requests, write responses via `send`,
    // and half-close via `close` when a request is not keep-alive.
    void processInput(const std::string& conn_name, buffer::Buffer* in,
                      const std::function<void(const std::string&)>& send,
                      const std::function<void()>& close);
    tls::TlsContextPtr tls_ctx_;  // null for plaintext
    HttpHandler handler_;
    ConnectionObserver connection_observer_;
    RequestObserver request_observer_;
    size_t max_body_bytes_{HttpContext::kDefaultMaxBodyBytes};

    // Per-connection state, keyed by connection name (accessed on the loop
    // thread). For TLS connections, holds the TlsConnection and a plaintext
    // reassembly buffer; for both, the HTTP parse context.
    struct PerConn;
    std::map<std::string, std::shared_ptr<PerConn>> conns_;

    // Declared LAST so it is destroyed FIRST: ~TcpServer fires connectDestroyed
    // on lingering connections, which calls back into onConnection() and
    // touches conns_. Tearing the server down before conns_ ensures those
    // callbacks see a valid map.
    net::TcpServer server_;
};
}  // namespace http
