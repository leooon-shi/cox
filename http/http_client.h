#pragma once

#include "http_request.h"
#include "http_response.h"
#include "inet_address.h"
#include "resolver.h"
#include "tls_context.h"

#include <functional>
#include <memory>
#include <string>

namespace reactor {
class EventLoop;
}

namespace http
{
class ConnectionPool;

// A simple HTTP/1.1 client for one origin, supporting keep-alive connection
// reuse and (optionally) TLS/HTTPS. Implemented as a single-connection
// ConnectionPool, so it shares the pool's proven keep-alive + TLS machinery.
//
// All callbacks fire on the provided EventLoop's thread. Use it sequentially
// (one request in flight); issue the next from the previous callback.
class HttpClient {
public:
    using ResponseCallback =
        std::function<void(const HttpResponse& resp, const std::string& error)>;

    // Plaintext HTTP client.
    HttpClient(reactor::EventLoop* loop, const net::InetAddress& server,
               std::string host_header = "");
    // HTTPS/mTLS client: tls_ctx is a client-role TlsContext.
    HttpClient(reactor::EventLoop* loop, const net::InetAddress& server,
               tls::TlsContextPtr tls_ctx, std::string host_header = "");
    
    // Hostname clients: `host` is resolved via `resolver` (which must outline
    // the client) before connecting, so callers need to resolve themselves.
    // The host header defaults to `host`.
    HttpClient(reactor::EventLoop* loop, const std::string& host, uint16_t port,
               net::Resolver* resolver, std::string host_header = "");
    HttpClient(reactor::EventLoop* loop, const std::string& host, uint16_t port,
               net::Resolver* resolver, tls::TlsContextPtr tls_ctx,
               std::string host_header = "");
    ~HttpClient();

    // Issue a request; the callback fires once with the response or an error.
    void send(const HttpRequest& req, ResponseCallback cb);

    // Convenience: GET a path.
    void get(const std::string& path, ResponseCallback cb);

private:
    std::shared_ptr<ConnectionPool> pool_;
};

}   // namespace http
