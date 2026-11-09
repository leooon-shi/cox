#pragma once

#include "inet_address.h"

#include <atomic>
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

    // (addresses, error). On success error is empty and addrs is non-empty,
    // ordered as the OS returned them (RFC 6724 / getaddrinfo order). On
    // failure addrs is empty and error is set. This is the dual-stack path:
    // the caller (Connector) tries each address in turn until one connects,
    // so a name that resolves to both an A and AAAA record reaches whichever
    // family is actually reachable.
    using ListCallback = std::function<void(
        const std::vector<InetAddress>& addrs, const std::string& error)>;

    virtual ~Resolver() = default;

    // Lookup outcome counters, shared by every backend (lifetime counts,
    // relaxed atomics, readable from any thread). One resolve()/resolveAll()
    // call counts exactly once: ok (addresses returned, including a cache hit)
    // or failed (error returned). Counted when the result is delivered on the
    // loop thread.
    uint64_t lookupsOk() const {
        return stats_->ok.load(std::memory_order_relaxed);
    }
    uint64_t lookupsFailed() const {
        return stats_->failed.load(std::memory_order_relaxed);
    }

protected:
    // Held by shared_ptr so a backend's in-flight work (which can outlive the
    // Resolver object) can still record into it safely.
    struct LookupStatus {
        std::atomic<uint64_t> ok{0};
        std::atomic<uint64_t> failed{0};
    };
    // Wrap a user callback so its innvocation records the outcome. Backends
    // apply this is resolveAll() before handling the callback to their Impl.
    ListCallback counted(ListCallback cb) const {
        return
            [stats = stats_, cb = std::move(cb)](
                const std::vector<InetAddress>& addrs, const std::string& err) {
                (err.empty() ? stats->ok : stats->failed)
                    .fetch_add(1, std::memory_order_relaxed);
                cb(addrs, err);
            };
    }

private:
    std::shared_ptr<LookupStatus> stats_ = std::make_shared<LookupStatus>();

public:
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
