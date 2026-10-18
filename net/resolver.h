#pragma once

#include "inet_address.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace reactor {
class EventLoop;
}

namespace concurrent {
class ThreadPool;
}

namespace net {
// Asynchronous DNS resolver interface. resolve() looks up `host` and invokes
// the callback (on the owning EventLoop's thread) with the resolved address or
// an error. Implementations must never block the event loop.
//
// Two backends (selected at build time / construction):
//   - ThreadPoolResolver: blocking getaddrinfo() offloaded to a worker pool.
//     Always available; the default.
//   - CaresResolver: async  c-ares (compiled only when COX_HAVE_CARES). Purely
//     fd-driven; no worker threads.
class Resolver{
public:
    // (address, error). On success error is empty and addr is valid.
    using Callback = 
        std::function<void(const InetAddress& addr, const std::string& error)>;

    virtual ~Resolver() = default;

    // Resolve host:port to an address, invoking cb on the loop thread.
    virtual void resolve(const std::string& host, uint16_t port,
                        Callback cb) = 0;
};

// getaddrinfo() offloaded to a worker ThreadPool; result posted back to the
// loop via runInLoop. Includes a small positive cache with TTL.
class ThreadPoolResolver : public Resolver {
public:
    ThreadPoolResolver(reactor::EventLoop* loop, concurrent::ThreadPool* pool);
    ~ThreadPoolResolver() override;

    void setCacheTtl( uint32_t seconds) { cache_ttl_s_ = seconds; }

    void resolve(const std::string& host, uint16_t port, Callback cb) override;
private:
    class Impl;
    std::shared_ptr<Impl> impl_;
    uint32_t cache_ttl_s_ {60};
}

} // namespace net