#pragma once

#include "connection_pool.h"
#include "http_request.h"
#include "http_response.h"
#include "inet_address.h"
#include "resolver.h"
#include "tls_context.h"

#include <cstdint>
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
// MultiOriginPool manages per-origin ConnectionPools behind one object, keyed
// by (host, port, tls). A client talking to several services uses one
// MultiOriginPool and routes each request to the right origin's sub-pool,
// created on demand. This mirrors Go's http.Transport (one transport, many
// hosts).
//
// Single-loop, like ConnectionPool. Per-origin limits/idle-timeout are applied
// to each sub-pool as it is created.
//
// DNS: without a resolver, origin hosts must be numeric IPs. Call setResolver()
// to accept hostnames -- each sub-pool then resolves its own name on first use.
class MultiOriginPool
{
public:
    using ResponseCallback = ConnectionPool::ResponseCallback;
    explicit MultiOriginPool(reactor::EventLoop* loop);
    MultiOriginPool(const MultiOriginPool&) = delete;
    MultiOriginPool& operator=(const MultiOriginPool&) = delete;

    // Resolve origin hostnames through `resolver` (must outline this pool).
    // Applies to sub-pools created after this call.
    void setResolver(net::Resolver* resolver) { resolver_ = resolver; }

    // Applied to each per-origin sub-pool created after this call.
    void setMaxConnectionsPerOrigin(size_t n) { max_per_origin_ = n; }
    void setIdleTimeout(uint32_t seconds) { idle_timeout_s_ = seconds; }

    // Register a TLS context to use for a given (host, port). Requests to that
    // origin will pool HTTPS/mTLS connections. Without a registered context an
    // origin is treated as plaintext HTTP.
    void setTlsContext(const std::string& host, uint16_t port,
                        tls::TlsContextPtr ctx);
    
    // Route a request to the origin's sub-pool (creating it on demand).
    // host_header defaults to the origin host if empty.
    void request(const std::string& host, uint16_t port, const HttpRequest& req, 
                 ResponseCallback cb, const std::string& host_header = "");
    
    size_t originCount() const { return pools_.size(); }

private:
    struct Origin {
        std::string host;
        uint16_t port;
        bool tls;
        bool operator<(const Origin& o) const {
            if (host != o.host) return host < o.host;
            if (port != o.port) return port < o.port;
            return tls < o.tls;
        }
    };

    reactor::EventLoop* loop_;
    net::Resolver* resolver_{nullptr};   // nullptr -> origins must be numberic IPs
    size_t max_per_origin_{8};
    uint32_t idle_timeout_s_{60};
    std::map<std::pair<std::string, uint16_t>, tls::TlsContextPtr> tls_ctx_;
    std::map<Origin, std::shared_ptr<ConnectionPool>> pools_;
};

} // namespace http