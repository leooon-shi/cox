#pragma once

#include "http_request.h"
#include "http_response.h"
#include "inet_address.h"
#include "tcp_server.h"
#include "tls_context.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace reactor
{
class EventLoop;
}

namespace http
{
// HttpServer is an HTTP/1.1 server built on net::TcpServer, optionally
// TLS-terminating (HTTPS). It gives each connection an HttpContext parser,
// dispatches each fully-parsed request to a user handler, serializes the
// response, and manages keep-alive.
//
// For HTTPS, construct with a server-role TlsContext; each accepted connection
// is wrapped in TLS and the handshake completes before requests are parsed.
class HttpServer
{
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
    void setConnectionObserver(ConnectionObserver obs)
    {
        connection_observer_ = std::move(obs);
    }

    void setKeepAliveTimeout(uint32_t seconds) { server_.setIdleTimeout(seconds); }
    void setMaxBodySize(size_t bytes) { server_.setMaxInputBufferSize(bytes); }

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
    tls::TlsContextPtr tls_ctx_;            // Null for plaintext
    HttpHandler handler_;
    ConnectionObserver connection_observer_;

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
}   // namespace http