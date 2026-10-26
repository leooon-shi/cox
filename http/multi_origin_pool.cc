#include "multi_origin_pool.h"

#include "event_loop.h"

#include <utility>

namespace http
{
MultiOriginPool::MultiOriginPool(reactor::EventLoop* loop)
    : loop_(loop)
{
}

void MultiOriginPool::setTlsContext(const std::string& host, uint16_t port,
                                    tls::TlsContextPtr ctx)
{
    tls_ctx_[{host, port}] = std::move(ctx);
}

void MultiOriginPool::request(const std::string& host, uint16_t port,
                              const HttpRequest& req, ResponseCallback cb,
                              const std::string& host_header)
{
    loop_->assertInLoopThread();

    auto tls_it = tls_ctx_.find({host, port});
    bool tls = (tls_it != tls_ctx_.end());
    Origin key{host, port, tls};

    auto it = pools_.find(key);
    if (it == pools_.end())
    {
        // Create the per-origin sub-pool on demand. With a resolver the host
        // may be a DNS name (the sub-pool resolves it on first request);
        // without one it must be a numeric IP.
        std::string hdr = host_header.empty() ? host : host_header;
        std::shared_ptr<ConnectionPool> pool;
        if (resolver_)
        {
            if (tls)
            {
                pool = std::make_shared<ConnectionPool>(
                    loop_, host, port, resolver_, tls_it->second, hdr);
            }
            else
            {
                pool = std::make_shared<ConnectionPool>(loop_, host, port,
                                                        resolver_, hdr);
            }
        }
        else
        {
            net::InetAddress addr(host, port);
            if (tls)
            {
                pool = std::make_shared<ConnectionPool>(loop_, addr,
                                                        tls_it->second, hdr);
            }
            else
            {
                pool = std::make_shared<ConnectionPool>(loop_, addr, hdr);
            }
        }
        pool->setMaxConnections(max_per_origin_);
        pool->setIdleTimeout(idle_timeout_s_);
        it = pools_.emplace(key, std::move(pool)).first;
    }
    it->second->request(req, std::move(cb));
}

}   // namespace http
