#include "connection_pool.h"

#include "buffer.h"
#include "event_loop.h"
#include "logger.h"
#include "response_parser.h"
#include "tcp_client.h"
#include "tcp_connection.h"
#include "tls_connection.h"
#include "tls_context.h"

#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace http {
namespace {
logging::Logger& log() {
    static logging::Logger logger;
    return logger;
}
}  // namespace

// One pooled connection: a TcpClient/connection (optionally wrapped in TLS)
// plus its parser and the callback for the request currently in flight.
class PooledConn : public std::enable_shared_from_this<PooledConn> {
public:
    using ResponseCallback = ConnectionPool::ResponseCallback;

    PooledConn(reactor::EventLoop* loop,
               const std::vector<net::InetAddress>& origins, std::string host,
               tls::TlsContextPtr tls_ctx)
        : client_(loop, origins, "pool-conn"),
          host_(std::move(host)),
          tls_ctx_(std::move(tls_ctx)) {}

    bool busy() const { return busy_; }
    bool connected() const { return ready_; }
    logging::Timestamp lastUsed() const { return last_used_; }

    std::function<void(const std::shared_ptr<PooledConn>&)> on_ready;
    std::function<void(const std::shared_ptr<PooledConn>&)> on_free;
    std::function<void(const std::shared_ptr<PooledConn>&)> on_closed;

    void init() {
        // WEAK, not strong: this object OWNs client_, so capturing a
        // shared_ptr to self in client_'s callbacks creates a reference cycle
        // (PooledConn -> TcpClient -> callback -> PooledConn) and the
        // connection
        // - along with its 1KB input/output Buffers - can never be freed.
        // LeakSanitizer found this as a set of indirect leaks with no direct
        // leak, which is the signature of a cycle. Lock the weak_ptr per event:
        // callbacks only fire while the loop is alive, and if the pool has
        // already dropped this connection there is nothing to do.
        std::weak_ptr<PooledConn> weak = weak_from_this();
        client_.setConnectionCallback([weak](const net::TcpConnectionPtr& c) {
            if (auto self = weak.lock()) {
                self->onConnection(c);
            }
        });
        client_.setMessageCallback(
            [weak](const net::TcpConnectionPtr& c, buffer::Buffer* b) {
                if (auto self = weak.lock()) {
                    self->onTcpMessage(c, b);
                }
            });
    }

    void connect() { client_.connect(); }

    void start(const HttpRequest& req, ResponseCallback cb) {
        busy_ = true;
        done_ = false;
        parser_.reset();
        callback_ = std::move(cb);
        HttpRequest r = req;
        r.addHeader("Connection", "keep-alive");
        buffer::Buffer out;
        r.appendTo(&out, host_);
        std::string wire = out.retrieveAllAsString();
        if (tls_conn_) {
            tls_conn_->send(wire);
        } else if (conn_) {
            conn_->send(wire);
        }
    }

    void close() {
        if (tls_conn_) {
            tls_conn_->shutdown();
        } else if (conn_) {
            conn_->shutdown();
        }
    }

private:
    ResponseCallback takeCallback() {
        ResponseCallback cb = std::move(callback_);
        callback_ = nullptr;
        return cb;
    }

    void onConnection(const net::TcpConnectionPtr& c) {
        if (c->connected()) {
            conn_ = c;
            last_used_ = logging::Timestamp::now();
            if (tls_ctx_) {
                // Wrap in TLS; ready only after the handshake completes.
                // host_ is passed as SNI so a multi-domain origin serves the
                // right certificate (and so the cert is checked against the
                // name we asked for).
                // WEAK for the same reason as in init(): this object owns
                // tls_conn_, so a strong self-capture in its callbacks would be
                // a cycle and leak the connection.
                std::weak_ptr<PooledConn> weak = weak_from_this();
                tls_conn_ = std::make_shared<tls::TlsConnection>(
                    c, tls_ctx_, tls::Role::kClient, host_);
                tls_conn_->setHandshakeDoneCallback(
                    [weak](const tls::TlsConnectionPtr&) {
                        auto self = weak.lock();
                        if (!self) return;
                        self->ready_ = true;
                        self->last_used_ = logging::Timestamp::now();
                        if (self->on_ready) self->on_ready(self);
                    });
                tls_conn_->setMessageCallback(
                    [weak](const tls::TlsConnectionPtr&, std::string& pt) {
                        if (auto self = weak.lock()) self->onPlaintext(pt);
                    });
                tls_conn_->start();
            } else {
                ready_ = true;
                if (on_ready) on_ready(shared_from_this());
            }
        } else {
            ready_ = false;
            conn_.reset();
            tls_conn_.reset();
            if (busy_ && !done_) {
                busy_ = false;
                done_ = true;
                ResponseCallback cb = takeCallback();
                parser_.onEof();
                if (parser_.gotAll()) {
                    if (cb) cb(parser_.response(), "");
                } else if (cb) {
                    cb(HttpResponse(), "connection closed before response");
                }
            }
            if (on_closed) on_closed(shared_from_this());
        }
    }

    // Plaintext (non-TLS) inbound bytes arrive as a Buffer.
    void onTcpMessage(const net::TcpConnectionPtr& c, buffer::Buffer* buf) {
        if (tls_conn_) return;  // TLS path handles bytes via TlsConnection
        consume(buf);
        (void)c;
    }

    // TLS inbound plaintext arrives as a string; feed it to a Buffer to parse.
    void onPlaintext(std::string& pt) {
        in_buf_.append(pt);
        consume(&in_buf_);
    }

    // Common: run the parser over `buf` and handle a completed response.
    void consume(buffer::Buffer* buf) {
        if (!parser_.parse(buf)) {
            ResponseCallback cb = takeCallback();
            if (cb) cb(HttpResponse(), "malformed response");
            close();
            return;
        }
        if (parser_.gotAll()) {
            HttpResponse resp = parser_.response();
            std::string conn_hdr = resp.header("Connection");
            for (auto& ch : conn_hdr) ch = static_cast<char>(::tolower(ch));
            bool server_keep_alive = (conn_hdr != "close");

            parser_.reset();
            busy_ = false;
            done_ = true;
            last_used_ = logging::Timestamp::now();

            ResponseCallback cb = takeCallback();
            if (server_keep_alive) {
                if (on_free) on_free(shared_from_this());
            } else {
                close();
            }
            if (cb) cb(resp, "");
        }
    }

    net::TcpClient client_;
    std::string host_;
    tls::TlsContextPtr tls_ctx_;  // null for plaintext
    net::TcpConnectionPtr conn_;
    tls::TlsConnectionPtr tls_conn_;  // set when TLS
    buffer::Buffer in_buf_;           // reassembles TLS plaintext
    ResponseParser parser_;
    ResponseCallback callback_;
    logging::Timestamp last_used_;
    bool ready_{false};
    bool busy_{false};
    bool done_{false};
};

// Pool internals.
class ConnectionPool::Impl : public std::enable_shared_from_this<Impl> {
public:
    Impl(reactor::EventLoop* loop, const net::InetAddress& origin,
         std::string host, tls::TlsContextPtr tls_ctx, size_t* max_conns,
         uint32_t* idle_s)
        : loop_(loop),
          origins_{origin},
          host_(std::move(host)),
          tls_ctx_(std::move(tls_ctx)),
          max_connections_(max_conns),
          idle_timeout_s_(idle_s),
          resolved_(true) {}

    // Hostname origin: the address is unknown until DNS resolves.
    Impl(reactor::EventLoop* loop, std::string dns_host, uint16_t port,
         net::Resolver* resolver, std::string host, tls::TlsContextPtr tls_ctx,
         size_t* max_conns, uint32_t* idle_s)
        : loop_(loop),
          host_(std::move(host)),
          tls_ctx_(std::move(tls_ctx)),
          max_connections_(max_conns),
          idle_timeout_s_(idle_s),
          resolved_(false),
          dns_host_(std::move(dns_host)),
          dns_port_(port),
          resolver_(resolver) {}

    bool usesTls() const { return tls_ctx_ != nullptr; }
    bool resolved() const { return resolved_; }

    struct Pending {
        HttpRequest req;
        ResponseCallback cb;
    };

    void startEviction() {
        // WEAK capture: this is a REPEATING timer that is never cancelled, so a
        // strong self-capture would keep the Impl -- and therefore every pooled
        // connection -- alive for the lifetime of the loop. That made the pool
        // leak by construction (LeakSanitizer reported the connections as
        // indirect leaks). With a weak_ptr the timer becomes a no-op once the
        // pool is gone.
        std::weak_ptr<Impl> weak = weak_from_this();
        eviction_timer_ = loop_->runEvery(5000, [weak]() {
            if (auto self = weak.lock()) self->evictIdle();
        });
        eviction_timer_armed_ = true;
    }

    // Cancel the eviction timer so the loop stops holding this callback.
    void stopEviction() {
        if (eviction_timer_armed_) {
            loop_->cancel(eviction_timer_);
            eviction_timer_armed_ = false;
        }
    }

    void request(const HttpRequest& req, ResponseCallback cb) {
        loop_->assertInLoopThread();
        // Hostname origin not yet resolved: park the request behind DNS.
        if (!resolved_) {
            waiting_on_dns_.push_back(Pending{req, std::move(cb)});
            startResolveOnce();
            return;
        }
        dispatch(req, std::move(cb));
    }

    size_t idleCount() const { return idle_.size(); }
    size_t busyCount() const { return busy_.size(); }
    size_t total() const {
        return idle_.size() + busy_.size() + connecting_.size();
    }
    size_t queued() const { return queue_.size() + waiting_on_dns_.size(); }

private:
    using ConnPtr = std::shared_ptr<PooledConn>;

    // Kick off DNS once; subsequent requests just queue behind it.
    void startResolveOnce() {
        if (resolving_) return;
        resolving_ = true;
        auto self = shared_from_this();
        resolver_->resolveAll(
            dns_host_, dns_port_,
            [self](const std::vector<net::InetAddress>& addrs,
                   const std::string& err) { self->onResolved(addrs, err); });
    }

    // Resolver callbacks arrive on the loop thread.
    void onResolved(const std::vector<net::InetAddress>& addr,
                    const std::string& err) {
        loop_->assertInLoopThread();
        resolving_ = false;
        std::deque<Pending> waiting = std::move(waiting_on_dns_);
        waiting_on_dns_.clear();

        if (!err.empty() || addrs.empty()) {
            // Fail every parked request; a later request retries resolution.
            std::string msg = err.empty() ? "no usable address" : err;
            log().error("DNS resolution failed for " + dns_host_ + ": " + msg);
            for (auto& p : waiting) {
                if (p.cb) p.cb(HttpResponse(), "DNS resolution failed: " + msg);
            }
            return;
        }

        // Keep the full, ordered address list; connections fail over between
        // them (dual-stack). dns_host_ stays as the SNI/Host value.
        origins_ = addrs;
        resolved_ = true;
        for (auto& p : waiting) {
            dispatch(p.req, std::move(p.cb));
        }
    }

    // Dispatch with a known origin address: reuse, open, or queue.
    void dispatch(const HttpRequest& req, ResponseCallback cb) {
        // Reuse an idle connection if available.
        if (!idle_.empty()) {
            auto c = idle_.back();
            idle_.pop_back();
            busy_.push_back(c);
            c->start(req, std::move(cb));
            return;
        }

        // Open a new connection if under the cap.
        if (total() < *max_connections_) {
            auto c =
                std::make_shared<PooledConn>(loop_, origins_, host_, tls_ctx_);
            wireCallbacks(c);
            c->init();
            connecting_.push_back(c);
            pending_[c.get()] = Pending{req, std::move(cb)};
            c->connect();
            return;
        }
        // All busy and at cap: queue.
        queue_.push_back(Pending{req, std::move(cb)});
    }

    void wireCallbacks(const ConnPtr& c) {
        // WEAK: Impl owns the connections (idle_/busy_/connecting_), so handing
        // each connection a strong ref back to Impl is a cycle -- neither side
        // is ever freed. This was the root of the pool's leak: LeakSanitizer
        // reported Impl, PooledConn and TcpConnection as indirect leaks with no
        // direct leak, which is the signature of a cycle rather than a missing
        // delete.
        std::weak_ptr<Impl> weak = weak_from_this();
        c->on_ready = [weak](const ConnPtr& conn) {
            if (auto self = weak.lock()) self->onReady(conn);
        };
        c->on_free = [weak](const ConnPtr& conn) {
            if (auto self = weak.lock()) self->onFree(conn);
        };
        c->on_closed = [weak](const ConnPtr& conn) {
            if (auto self = weak.lock()) self->onClosed(conn);
        };
    }

    void removeFrom(std::vector<ConnPtr>& v, const ConnPtr& c) {
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (*it == c) {
                v.erase(it);
                return;
            }
        }
    }

    void onReady(const ConnPtr& c) {
        loop_->assertInLoopThread();
        removeFrom(connecting_, c);
        auto it = pending_.find(c.get());
        if (it != pending_.end()) {
            busy_.push_back(c);
            Pending p = std::move(it->second);
            pending_.erase(it);
            c->start(p.req, std::move(p.cb));
        } else {
            idle_.push_back(c);  // no pending req (shouldn't normally happen)
        }
    }

    void onFree(const ConnPtr& c) {
        loop_->assertInLoopThread();
        removeFrom(busy_, c);
        // Serve a queued request on this now-free connection, else idle it.
        if (!queue_.empty()) {
            Pending p = std::move(queue_.front());
            queue_.pop_front();
            busy_.push_back(c);
            c->start(p.req, std::move(p.cb));
        } else {
            idle_.push_back(c);
        }
    }

    void onClosed(const ConnPtr& c) {
        loop_->assertInLoopThread();
        removeFrom(idle_, c);
        removeFrom(busy_, c);
        removeFrom(connecting_, c);
        pending_.erase(c.get());
        // If there are queued requests and capacity, open a replacement.
        if (!queue_.empty() && total() < *max_connections_) {
            Pending p = std::move(queue_.front());
            queue_.pop_front();
            auto nc =
                std::make_shared<PooledConn>(loop_, origins_, host_, tls_ctx_);
            wireCallbacks(nc);
            nc->init();
            connecting_.push_back(nc);
            pending_[nc.get()] = std::move(p);
            nc->connect();
        }
    }

    void evictIdle() {
        loop_->assertInLoopThread();
        auto now = logging::Timestamp::now();
        std::vector<ConnPtr> to_close;
        for (auto& c : idle_) {
            uint64_t idle_ms = (now.microSecondsSinceEpoch() -
                                c->lastUsed().microSecondsSinceEpoch()) /
                               1000;
            if (idle_ms > static_cast<uint64_t>(*idle_timeout_s_) * 1000) {
                to_close.push_back(c);
            }
        }

        for (auto& c : to_close) {
            removeFrom(idle_, c);
            c->close();  // onClosed will fully remove it.
        }
    }

    reactor::EventLoop* loop_;
    std::vector<net::InetAddress>
        origins_;  // candidate addresses (>=1 once resolved)
    std::string host_;
    tls::TlsContextPtr tls_ctx_;
    size_t* max_connections_;
    uint32_t* idle_timeout_s_;

    std::vector<ConnPtr> idle_;
    std::vector<ConnPtr> busy_;
    std::vector<ConnPtr> connecting_;
    std::map<PooledConn*, Pending> pending_;  // connecting -> its request
    std::deque<Pending> queue_;               // waiting for a free connection

    // DNS state (hostname origins only). resolved_ is true from construction
    // for numeric origins, so those pay nothing for this path.
    bool resolved_{true};
    bool resolving_{false};
    std::string dns_host_;
    uint16_t dns_port_{0};
    net::Resolver* resolver_{nullptr};
    std::deque<Pending> waiting_on_dns_;  // parked until the name resolves

    reactor::TimerId eviction_timer_{};
    bool eviction_timer_armed_{false};
};

// ---- ConnectionPool facade ----

ConnectionPool::ConnectionPool(reactor::EventLoop* loop,
                               const net::InetAddress& origin,
                               std::string host_header) {
    impl_ = std::make_shared<Impl>(loop, origin, std::move(host_header),
                                   /*tls_ctx*/ nullptr, &max_connections_,
                                   &idle_timeout_s_);
    impl_->startEviction();
}

ConnectionPool::ConnectionPool(reactor::EventLoop* loop,
                               const net::InetAddress& origin,
                               tls::TlsContextPtr tls_ctx,
                               std::string host_header) {
    impl_ = std::make_shared<Impl>(loop, origin, std::move(host_header),
                                   std::move(tls_ctx), &max_connections_,
                                   &idle_timeout_s_);
    impl_->startEviction();
}

ConnectionPool::ConnectionPool(reactor::EventLoop* loop,
                               const std::string& host, uint16_t port,
                               net::Resolver* resolver,
                               std::string host_header) {
    std::string hdr = host_header.empty() ? host : std::move(host_header);
    impl_ = std::make_shared<Impl>(loop, host, port, resolver, std::move(hdr),
                                   /*tls_ctx=*/nullptr, &max_connections_,
                                   &idle_timeout_s_);
    impl_->startEviction();
}

ConnectionPool::ConnectionPool(reactor::EventLoop* loop,
                               const std::string& host, uint16_t port,
                               net::Resolver* resolver,
                               tls::TlsContextPtr tls_ctx,
                               std::string host_header) {
    std::string hdr = host_header.empty() ? host : std::move(host_header);
    impl_ = std::make_shared<Impl>(loop, host, port, resolver, std::move(hdr),
                                   std::move(tls_ctx), &max_connections_,
                                   &idle_timeout_s_);
    impl_->startEviction();
}
ConnectionPool::~ConnectionPool() {
    // Stop the eviction timer so the loop drops its reference to Impl;
    // otherwise the pool outlives the ConnectionPool facade.
    if (impl_) impl_->stopEviction();
}

void ConnectionPool::request(const HttpRequest& req, ResponseCallback cb) {
    impl_->request(req, std::move(cb));
}

size_t ConnectionPool::idleCount() const {
    return impl_->idleCount();
}

size_t ConnectionPool::busyCount() const {
    return impl_->busyCount();
}

size_t ConnectionPool::totalConnections() const {
    return impl_->total();
}

size_t ConnectionPool::queuedRequests() const {
    return impl_->queued();
}

bool ConnectionPool::usesTls() const {
    return impl_->usesTls();
}

bool ConnectionPool::resolved() const {
    return impl_->resolved();
}

}  // namespace http
