#include "resolver.h"

#include "event_loop.h"
#include "logger.h"
#include "thread_pool.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace net {
namespace {
logging::Logger& log() {
    static logging::Logger logger;
    return logger;
}

// Blocking resolution via getaddrinfo. Returns 0 on success and fills `out`
// with every usable (IPv4/IPv6) address, in the order getaddrinfo returned
// them (the OS applies RFC 6724 destination-address ordering). On failure
// returns the getaddrinfo error code and sets `err`.
int resolveBlocking(const std::string& host, uint16_t port,
                    std::vector<InetAddress>* out, std::string* err) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;  // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0) {
        *err = ::gai_strerror(rc);
        return rc;
    }

    // Collect ALL results, preserving order, so the caller can fail over
    // between families/addresses. Taking only the first entry (as an earlier
    // version did) meant an IPv6-first host committed to ::1 and could never
    // reach an IPv4-only server -- the dual-stack bug this path fixes.
    for(struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        if (res->ai_family == AF_INET) {
            auto* sin = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
            sin->sin_port = htons(port);
            out->push_back(InetAddress(*sin));
        } else if (res->ai_family == AF_INET6) {
            auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(res->ai_addr);
            sin6->sin6_port = htons(port);
            out->push_back(InetAddress(*sin6));
        }
        // Silently skip faimilies we do not use (e.g. AF_UNIX never appears).
    }

    ::freeaddrinfo(res);

    if (out->empty())
    {
        *err = "no usable address";
        return -1;
    }
    return 0;
}
}  // namespace

class ThreadPoolResolver::Impl : public std::enable_shared_from_this<Impl> {
public:
    Impl(reactor::EventLoop* loop, concurrent::ThreadPool* pool)
        : loop_(loop), pool_(pool) {}

    struct CacheEntry {
        std::vector<InetAddress> addrs;
        logging::Timestamp when;
    };

    void resolve(const std::string& host, uint16_t port, ListCallback cb,
                 uint32_t ttl_s) {
        // Cache hit? (positive cache, TTL-bounded)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(host);
            if (it != cache_.end()) {
                uint64_t age_s =
                    (logging::Timestamp::now().microSecondsSinceEpoch() -
                     it->second.when.microSecondsSinceEpoch()) /
                    1000000;

                if (age_s <= ttl_s) {
                    // Re-stamp the requested port (cache keyed by host only).
                    std::vector<InetAddress> result;
                    result.reserve(it->second.addrs.size());
                    for (const auto& a : it->second.addrs) {
                        result.push_back(withPort(a, port));
                    }
                    loop_->runInLoop([cb, result]() { cb(result, ""); });
                    return;
                }
            }
        }

        // Miss: offload the blocking lookup to the worker pool.
        // Note: ttl_s is NOT captured -- expiry is evaluated on lookup (above)
        // against the entry's timestamp, so the worker only needs to stamp it.
        auto self = shared_from_this();

        std::string h = host;
        pool_->run([self, h, port, cb]() {
            std::vector<InetAddress> addrs;
            std::string err;
            int rc = resolveBlocking(h, port, &addrs, &err);
            if (rc == 0) {
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    self->cache_[h] =
                        CacheEntry{addrs, logging::Timestamp::now()};
                }
                self->loop_->runInLoop([cb, addrs]() { cb(addrs, ""); });
            } else {
                // Log the failure here: the callback only receives the message
                // string, so without this a resolution failure leaves no trace
                // on the server side.
                log().debug("DNS resolution failed for " + h + ": " + err);
                self->loop_->runInLoop(
                    [cb, err]() { cb(std::vector<InetAddress>(), err); });
            }
        });
    }

private:
    static InetAddress withPort(const InetAddress& a, uint16_t port) {
        // Reconstruct with the desired port from the cached IP string.
        if (a.isIpv6()) {
            return InetAddress(a.toIp(), port, /*ipv6=*/true);
        }
        return InetAddress(a.toIp(), port);
    }
    reactor::EventLoop* loop_;
    concurrent::ThreadPool* pool_;
    std::mutex mutex_;
    std::map<std::string, CacheEntry> cache_;
};

ThreadPoolResolver::ThreadPoolResolver(reactor::EventLoop* loop,
                                       concurrent::ThreadPool* pool)
    : impl_(std::make_shared<Impl>(loop, pool)) {}

ThreadPoolResolver::~ThreadPoolResolver() = default;

void ThreadPoolResolver::resolve(const std::string& host, uint16_t port,
                                 Callback cb) {
    // One-address convenience: delegate to the list path and hand back the
    // first result (or the error).
    resolveAll(host, port,
        [cb = std::move(cb)](const std::vector<InetAddress>& addrs,
                                                const std::string& err) {
            if (err.empty()) {
                cb(addrs.front(), "");
            } else {
                cb(InetAddress(), err);
            }
        });
}

void ThreadPoolResolver::resolveAll(const std::string& host, uint16_t port,
                                 ListCallback cb) {
    impl_->resolve(host, port, std::move(cb), cache_ttl_s_);
}

}  // namespace net
