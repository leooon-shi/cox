#pragma once

#include "inet_address.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
//   - CaresResolver: async c-ares (compiled only when COX_HAVE_CARES). Purely
//     fd-driven; no worker threads.
class Resolver {
public:
    // (address, error). On success error is empty and addr is valid.
    using Callback =
        std::function<void(const InetAddress& addr, const std::string& error)>;

    // (addresses, error). On Success error is empty and addrs is non-empty,
    // ordered as the OS returned them (RFC 6742 / getaddrinfo order). On
    // failure addrs is empty and error is set. This is the dual-stack path:
    // the caller (Connector) tries each address in turn until one connects,
    // so a name that resolves to both an A and AAA record reaches whichever
    // faimily is actually reachable.
    using ListCallback = std::function<void(
        const std::vector<InetAddress>& addrs, const std::string& err)>;

    virtual ~Resolver() = default;

    // Resolve host:port to an address, invoking cb on the loop thread. Kept
    // for callers that only need one address; implemented in terms of
    // resolveAll(), handing back the first result.
    virtual void resolve(const std::string& host, uint16_t port,
                         Callback cb) = 0;
    
    // Resolve host:port to the full ordered address list, invoking cb on the
    // loop thread.
    virtual void resolveAll(const std::string& host, uint16_t port,
                            ListCallback cb) = 0;
};

// getaddrinfo() offloaded to a worker ThreadPool; result posted back to the
// loop via runInLoop. Includes a small positive cache with TTL.
class ThreadPoolResolver : public Resolver {
public:
    ThreadPoolResolver(reactor::EventLoop* loop, concurrent::ThreadPool* pool);
    ~ThreadPoolResolver() override;

    void setCacheTtl(uint32_t seconds) { cache_ttl_s_ = seconds; }

    void resolve(const std::string& host, uint16_t port, Callback cb) override;
    void resolveAll(const std::string& host, uint16_t port,
                    ListCallback cb) override;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
    uint32_t cache_ttl_s_{60};
};

}  // namespace net
