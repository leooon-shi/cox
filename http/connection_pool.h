#pragma once

#include "http_request.h"
#include "http_response.h"
#include "inet_address.h"
#include "resolver.h"
#include "tls_context.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace reactor
{
class EventLoop;
}

namespace http
{
// An HTTP/1.1 connection pool to a single origin (host:port), optionally over
// TLS. It maintains a set of reusable keep-alive connections, dispatches
// requests over idle ones (opening new connections up to a cap), returns
// connections to the idle set after each response, evicts connections idle 
// beyond a timeout, and queues requests when all connections are busy and the
// cap is reached.
//
// TLS: pass a TlsContext (client role) to pool HTTPS/mTLS connections. Pooling
// amortizes the TLS handshake across many requests -- the primary performance
// reason to pool for HTTPS. A connection becomes poolable only after its
// handshake completes.
//
// DNS: the origin can be given either as a numeric net::InetAddress or as a
// hostname + port + net::Resolver. In the latter case the name is resolved once
// (asynchronously, on first request) and the resulting address is used for all
// connections; requests arriving during resolution are queued.
//
// Single-loop by design: all pooled connections live on one EventLoop, so
// concurrency comes from multiple connections on that loop (not threads) and
// the pool needs no locking. Create one pool per origin.
class ConnectionPool : public std::enable_shared_from_this<ConnectionPool> {
public:
    using ResponseCallback = 
        std::function<void(const HttpResponse& resp, const std::string& error)>;
    // Plaintext (HTTP) pool.
    ConnectionPool(reactor::EventLoop* loop, const net::InetAddress& origin,
                    std::string host_header = "");
    // TLS (HTTPS/mTLS) pool: tls_ctx must be a client-role TlsContext.
    ConnectionPool(reactor::EventLoop* loop, const net::InetAddress& origin,
                    tls::TlsContextPtr tls_ctx, std::string host_header = "");
    // Hostname pools: the origin is given as a DNS name and resolved through
    // `resolver` (which must outlive the pool) before the first connection.
    // Requests issued while resolution is in flight are queued; if resolution
    // fails they all complete with that error. host_header defaults to `host`.
    ConnectionPool(reactor::EventLoop* loop, const std::string& host,
                    uint16_t port, net::Resolver* resolver,
                    std::string host_header = "");
    ConnectionPool(reactor::EventLoop* loop, const std::string& host,
                    uint16_t port, net::Resolver* resolver,
                    tls::TlsContextPtr tls_ctx, std::string host_header = "");
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    void setMaxConnections(size_t n) { max_connections_ = n; }
    void setIdleTimeout(uint32_t seconds) { idle_timeout_s_ = seconds; }

    void request(const HttpRequest& req, ResponseCallback cb);

    size_t idleCount() const;
    size_t busyCount() const;
    size_t totalConnections() const;
    size_t queuedRequests() const;
    bool usesTls() const;
    // True once the origin address is known (immediately for numeric origins,
    // after successful DNS resolution for hostname origins).
    bool resolved() const;
private:
    class Impl;
    std::shared_ptr<Impl> impl_;

    size_t max_connections_{8};
    uint32_t idle_timeout_s_{60};
};

}    // namespace http
