#include "http2_client.h"

#include "event_loop.h"
#include "http2_client_session.h"
#include "log_macros.h"
#include "logger.h"
#include "tcp_client.h"
#include "tcp_connection.h"
#include "tls_connection.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace http {
COX_LOG_DEFINE();

// One TCP + TLS + HTTP/2 connection. Owned by Http2Client::Impl. Callbacks
// into it capture weak pointers (it owns the TcpClient and TlsConnection, so
// strong captures would be ownership cycles -- the leak class LSan found in
// ConnectionPool).
class H2Conn : public std::enable_shared_from_this<H2Conn> {
public:
    using Result = Http2ClientSession::Result;
    using ResultCallback = Http2ClientSession::ResponseCallback;

    // `servers`: candidate addresses tried in order (dual-stack failover).
    H2Conn(reactor::EventLoop* loop,
           const std::vector<net::InetAddress>& servers, tls::TlsContextPtr ctx,
           std::string host, Http2ClientSession::Limits limits, uint64_t id,
           uint32_t connect_timeout_ms)
        : loop_(loop),
          client_(loop, servers, "h2-client#" + std::to_string(id)),
          ctx_(std::move(ctx)),
          host_(std::move(host)),
          limits_(limits),
          connect_timeout_ms_(connect_timeout_ms) {}

    ~H2Conn() {
        if (timer_armed_) {
            loop_->cancel(connect_timer_);
        }
        client_.stop();
    }

    // Called when this connection stops accepting requests (GOAWAY, IDs
    // exhausted, closed). The owner then routes new requests elsewhere.
    std::function<void()> on_unusable;
    // Called once when the TCP connection is gone.
    std::function<void()> on_closed;

    void start() {
        std::weak_ptr<H2Conn> weak = weak_from_this();
        client_.setConnectionCallback([weak](const net::TcpConnectionPtr& c) {
            if (auto self = weak.lock()) {
                self->onConnection(c);
            }
        });
        if (connect_timeout_ms_ > 0) {
            timer_armed_ = true;
            connect_timer_ = loop_->runAfter(connect_timeout_ms_, [weak] {
                if (auto self = weak.lock()) {
                    self->timer_armed_ = false;
                    self->onConnectTimeout();
                }
            });
        }
        client_.connect();
    }

    // Usable for NEW requests: connecting, or ready and not draining.
    bool usable() const {
        return !closed_ && (!session_ || session_->canAcceptRequests());
    }
    bool idle() const { return load() == 0; }
    // True once TLS + ALPN h2 succeeded at least once on this connection.
    bool everReady() const { return ready_; }
    // Requests on this connection (in flight + queued + waiting for it to
    // connect): the pool's load measure.
    size_t load() const {
        size_t n = early_.size();
        if (session_) {
            n += session_->inFlight() + session_->queued();
        }
        return n;
    }
    // True while this connection can open a stream right away: still
    // connecting (limit not known yet) or below the server's
    // MAX_CONCURRENT_STREAMS. The pool prefers these.
    bool hasFreeStream() const {
        return !session_ || session_->hasFreeStream();
    }

    // Queue until ready, then hand to the session.
    void submit(const HttpRequest& req, ResultCallback cb) {
        if (session_ && ready_) {
            session_->submit(req, host_, "https", std::move(cb));
            return;
        }
        early_.push_back({req, std::move(cb)});
    }

    void shutdown() {
        if (session_) {
            session_->shutdown();
        }
    }

    // Close if nothing is outstanding (after GOAWAY/drain).
    void closeIfDone() {
        if (tls_ && session_ && !session_->canAcceptRequests() &&
            session_->finished()) {
            tls_->shutdown();
        }
    }

private:
    struct Early {
        HttpRequest req;
        ResultCallback cb;
    };

    void onConnection(const net::TcpConnectionPtr& c) {
        if (c->connected()) {
            std::weak_ptr<H2Conn> weak = weak_from_this();
            tls_ = std::make_shared<tls::TlsConnection>(
                c, ctx_, tls::Role::kClient, host_);
            tls_->setHandshakeDoneCallback(
                [weak](const tls::TlsConnectionPtr& t) {
                    if (auto self = weak.lock()) {
                        self->onSecure(t);
                    }
                });
            tls_->setMessageCallback(
                [weak](const tls::TlsConnectionPtr&, std::string& pt) {
                    auto self = weak.lock();
                    if (!self || !self->session_) {
                        return;
                    }
                    if (!self->session_->feed(pt.data(), pt.size())) {
                        COX_LOG_DEBUG("h2 client protocol error: " +
                                      self->session_->lastError());
                        self->tls_->shutdown();
                    }
                    self->closeIfDone();
                });
            c->setWriteCompleteCallback([weak](const net::TcpConnectionPtr&) {
                if (auto self = weak.lock()) {
                    if (self->session_) {
                        self->session_->flush();
                    }
                }
            });
            tls_->start();
            return;
        }
        // Disconnected (or the connect never succeeded).
        closed_ = true;
        if (session_) {
            session_->connectionLost("connection closed");
        }
        failEarly("connection closed before HTTP/2 was ready");
        if (on_unusable) {
            on_unusable();
        }
        if (on_closed) {
            on_closed();
        }
    }

    void onSecure(const tls::TlsConnectionPtr& t) {
        if (t->negotiatedProtocol() != "h2") {
            COX_LOG_WARNING("server did not negotiate h2 (ALPN '" +
                            t->negotiatedProtocol() + "')");
            closed_ = true;
            failEarly("server does not support HTTP/2 (ALPN)",
                      /*retryable=*/false);
            if (on_unusable) {
                on_unusable();
            }
            t->shutdown();
            return;
        }
        std::weak_ptr<H2Conn> weak = weak_from_this();
        tls::TlsConnection* raw_tls = t.get();
        net::TcpConnection* raw_tcp = t->tcpConnection().get();
        Http2ClientSession::Callbacks cbs;
        // Raw pointers: the session is owned by this H2Conn, which also owns
        // tls_ (and through it the TcpConnection), so they outlive it.
        cbs.send = [raw_tls](const char* d, size_t n) {
            raw_tls->send(std::string(d, n));
        };
        cbs.pending = [raw_tcp] { return raw_tcp->outputBufferPending(); };
        cbs.on_draining = [weak] {
            if (auto self = weak.lock()) {
                if (self->on_unusable) {
                    self->on_unusable();
                }
            }
        };
        session_ =
            std::make_unique<Http2ClientSession>(std::move(cbs), limits_);
        if (!session_->start()) {
            COX_LOG_ERROR("h2 client session init failed: " +
                          session_->lastError());
            closed_ = true;
            failEarly("HTTP/2 session init failed");
            t->shutdown();
            return;
        }
        ready_ = true;
        if (timer_armed_) {
            loop_->cancel(connect_timer_);
            timer_armed_ = false;
        }
        std::vector<Early> early;
        early.swap(early_);
        for (auto& e : early) {
            session_->submit(e.req, host_, "https", std::move(e.cb));
        }
    }

    void failEarly(const std::string& why, bool retryable = true) {
        std::vector<Early> early;
        early.swap(early_);
        for (auto& e : early) {
            e.cb(Result{HttpResponse(), why, retryable});
        }
    }

    void onConnectTimeout() {
        if (ready_ || closed_) {
            return;
        }
        COX_LOG_WARNING("h2 client: connect timed out after " +
                        std::to_string(connect_timeout_ms_) + " ms");
        closed_ = true;
        client_.stop();  // stop the connector's retry loop
        if (tls_) {
            tls_->tcpConnection()->forceClose();
        }
        // Not retryable: the next connection would most likely time out too,
        // and retrying would multiply the wait.
        failEarly("connect timed out", /*retryable=*/false);
        if (on_unusable) {
            on_unusable();
        }
        if (on_closed) {
            on_closed();
        }
    }

    reactor::EventLoop* loop_;
    net::TcpClient client_;
    tls::TlsContextPtr ctx_;
    std::string host_;
    Http2ClientSession::Limits limits_;
    uint32_t connect_timeout_ms_;
    reactor::TimerId connect_timer_;
    bool timer_armed_{false};
    tls::TlsConnectionPtr tls_;
    std::unique_ptr<Http2ClientSession> session_;
    std::vector<Early> early_;
    bool ready_{false};
    bool closed_{false};
};

class Http2Client::Impl : public std::enable_shared_from_this<Impl> {
public:
    // Numeric origin.
    Impl(reactor::EventLoop* loop, const net::InetAddress& server,
         tls::TlsContextPtr ctx, std::string host, Options opts)
        : loop_(loop),
          servers_{server},
          ctx_(std::move(ctx)),
          host_(host.empty() ? server.toIp() : std::move(host)),
          opts_(opts),
          resolved_(true) {
        ctx_->setAlpnProtocols({"h2"});
    }

    // Hostname origin: resolved through `resolver` before the first
    // connection, re-resolved when every address fails.
    Impl(reactor::EventLoop* loop, std::string dns_host, uint16_t port,
         net::Resolver* resolver, tls::TlsContextPtr ctx, std::string host,
         Options opts)
        : loop_(loop),
          ctx_(std::move(ctx)),
          host_(host.empty() ? dns_host : std::move(host)),
          opts_(opts),
          resolved_(false),
          dns_host_(std::move(dns_host)),
          dns_port_(port),
          resolver_(resolver) {
        ctx_->setAlpnProtocols({"h2"});
    }

    ~Impl() {
        // Connections hold only weak references back here; dropping them
        // tears down sockets and fails outstanding requests ("client
        // destroyed" via the sessions' destructors).
        active_.clear();
        retiring_.clear();
        failDnsWaiters("client destroyed");
    }

    void send(const HttpRequest& req, ResponseCallback cb, int attempt) {
        if (stopped_) {
            cb(HttpResponse(), "client shut down");
            return;
        }
        if (!resolved_) {
            // Park behind DNS; the first parked request starts the lookup.
            dns_waiters_.push_back(Parked{req, std::move(cb), attempt});
            startResolve();
            return;
        }
        std::shared_ptr<H2Conn> conn = pick();
        std::weak_ptr<Impl> weak = weak_from_this();
        conn->submit(req, [weak, req, cb = std::move(cb),
                           attempt](H2Conn::Result r) mutable {
            auto self = weak.lock();
            if (!self) {
                cb(HttpResponse(),
                   r.error.empty() ? "client destroyed" : r.error);
                return;
            }
            if (r.error.empty()) {
                cb(r.response, "");
            } else if (r.retryable && attempt < self->opts_.max_retries &&
                       !self->stopped_) {
                ++self->retries_;
                // Not processed by the server: safe to re-send. Defer so the
                // session that failed it finishes its own bookkeeping first.
                self->loop_->queueInLoop(
                    [weak, req, cb = std::move(cb), attempt]() mutable {
                        if (auto s = weak.lock()) {
                            s->send(req, std::move(cb), attempt + 1);
                        } else {
                            cb(HttpResponse(), "client destroyed");
                        }
                    });
            } else {
                cb(HttpResponse(), r.error);
            }
            self->reap();
        });
    }

    void shutdown() {
        stopped_ = true;
        failDnsWaiters("client shut down");
        std::vector<std::shared_ptr<H2Conn>> active;
        active.swap(active_);
        for (auto& c : active) {
            c->shutdown();
            retire(c);
        }
        reap();
    }

    uint64_t opened() const { return opened_; }
    uint64_t retries() const { return retries_; }
    size_t openConnections() const { return active_.size() + retiring_.size(); }

private:
    struct Parked {
        HttpRequest req;
        ResponseCallback cb;
        int attempt;
    };

    // Choose a connection for a new request:
    //   1. the least-loaded active connection that can open a stream now;
    //   2. otherwise a NEW connection, while under max_connections;
    //   3. otherwise the least-loaded active connection (the request queues
    //      there until one of its streams finishes).
    std::shared_ptr<H2Conn> pick() {
        // Drop connections that stopped accepting requests (GOAWAY, stream
        // IDs exhausted, closed); they finish their own streams as retiring.
        for (size_t i = 0; i < active_.size();) {
            if (!active_[i]->usable()) {
                retire(active_[i]);
                active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        std::shared_ptr<H2Conn> best_free;
        std::shared_ptr<H2Conn> least;
        for (const auto& c : active_) {
            if (c->hasFreeStream() &&
                (!best_free || c->load() < best_free->load())) {
                best_free = c;
            }
            if (!least || c->load() < least->load()) {
                least = c;
            }
        }
        if (best_free) {
            return best_free;
        }
        if (active_.size() < std::max<size_t>(1, opts_.max_connections)) {
            return openConnection();
        }
        return least;
    }

    std::shared_ptr<H2Conn> openConnection() {
        Http2ClientSession::Limits lim;
        lim.max_queued = opts_.max_queued;
        lim.max_response_body = opts_.max_response_body;
        lim.initial_window_size = opts_.initial_window_size;
        lim.max_stream_id = opts_.max_stream_id;
        auto conn =
            std::make_shared<H2Conn>(loop_, servers_, ctx_, host_, lim,
                                     ++opened_, opts_.connect_timeout_ms);
        std::weak_ptr<Impl> weak = weak_from_this();
        std::weak_ptr<H2Conn> wconn = conn;
        conn->on_unusable = [weak, wconn] {
            auto self = weak.lock();
            auto c = wconn.lock();
            if (!self || !c) {
                return;
            }
            // Further requests go elsewhere; this one keeps serving its
            // in-flight streams until they finish.
            auto it = std::find(self->active_.begin(), self->active_.end(), c);
            if (it != self->active_.end()) {
                self->active_.erase(it);
                self->retire(c);
            }
        };
        conn->on_closed = [weak, wconn] {
            auto self = weak.lock();
            auto c = wconn.lock();
            if (!self || !c) {
                return;
            }
            if (!c->everReady() && self->resolver_ != nullptr) {
                // Never connected: the cached addresses may be stale.
                self->resolved_ = false;
            }
            // Drop it on the next loop turn, not inside its own callback.
            self->loop_->queueInLoop([weak, wconn] {
                auto s = weak.lock();
                auto cc = wconn.lock();
                if (!s || !cc) {
                    return;
                }
                auto erase = [&cc](std::vector<std::shared_ptr<H2Conn>>& v) {
                    v.erase(std::remove(v.begin(), v.end(), cc), v.end());
                };
                erase(s->active_);
                erase(s->retiring_);
            });
        };
        conn->start();
        active_.push_back(conn);
        return conn;
    }

    void startResolve() {
        if (resolving_) {
            return;
        }
        resolving_ = true;
        std::weak_ptr<Impl> weak = weak_from_this();
        resolver_->resolveAll(dns_host_, dns_port_,
                              [weak](const std::vector<net::InetAddress>& addrs,
                                     const std::string& err) {
                                  if (auto self = weak.lock()) {
                                      self->onResolved(addrs, err);
                                  }
                              });
    }

    void onResolved(const std::vector<net::InetAddress>& addrs,
                    const std::string& err) {
        resolving_ = false;
        if (!err.empty() || addrs.empty()) {
            const std::string msg = err.empty() ? "no usable address" : err;
            COX_LOG_ERROR("h2 client: DNS resolution failed for " + dns_host_ +
                          ": " + msg);
            // Every parked request fails; a later request retries DNS.
            failDnsWaiters("DNS resolution failed: " + msg);
            return;
        }
        // The full ordered list: each connection fails over across it
        // (dual-stack), like ConnectionPool.
        servers_ = addrs;
        resolved_ = true;
        std::vector<Parked> waiting;
        waiting.swap(dns_waiters_);
        for (auto& p : waiting) {
            send(p.req, std::move(p.cb), p.attempt);
        }
    }

    void failDnsWaiters(const std::string& why) {
        std::vector<Parked> waiting;
        waiting.swap(dns_waiters_);
        for (auto& p : waiting) {
            p.cb(HttpResponse(), why);
        }
    }

    void retire(const std::shared_ptr<H2Conn>& c) {
        if (std::find(retiring_.begin(), retiring_.end(), c) ==
            retiring_.end()) {
            retiring_.push_back(c);
        }
    }

    // Close retiring connections whose streams have all finished.
    void reap() {
        std::vector<std::shared_ptr<H2Conn>> list = retiring_;
        for (auto& c : list) {
            c->closeIfDone();
        }
    }

    reactor::EventLoop* loop_;
    std::vector<net::InetAddress> servers_;  // candidate addresses
    tls::TlsContextPtr ctx_;
    std::string host_;
    Options opts_;
    std::vector<std::shared_ptr<H2Conn>> active_;    // accept new requests
    std::vector<std::shared_ptr<H2Conn>> retiring_;  // draining
    uint64_t opened_{0};
    uint64_t retries_{0};
    bool stopped_{false};
    // DNS (hostname origins only).
    bool resolved_;
    bool resolving_{false};
    std::string dns_host_;
    uint16_t dns_port_{0};
    net::Resolver* resolver_{nullptr};
    std::vector<Parked> dns_waiters_;
};

Http2Client::Http2Client(reactor::EventLoop* loop,
                         const net::InetAddress& server,
                         tls::TlsContextPtr tls_ctx, std::string host_header)
    : Http2Client(loop, server, std::move(tls_ctx), std::move(host_header),
                  Options{}) {}

Http2Client::Http2Client(reactor::EventLoop* loop,
                         const net::InetAddress& server,
                         tls::TlsContextPtr tls_ctx, std::string host_header,
                         Options options)
    : impl_(std::make_shared<Impl>(loop, server, std::move(tls_ctx),
                                   std::move(host_header), options)) {}

Http2Client::Http2Client(reactor::EventLoop* loop, const std::string& host,
                         uint16_t port, net::Resolver* resolver,
                         tls::TlsContextPtr tls_ctx, std::string host_header)
    : Http2Client(loop, host, port, resolver, std::move(tls_ctx),
                  std::move(host_header), Options{}) {}

Http2Client::Http2Client(reactor::EventLoop* loop, const std::string& host,
                         uint16_t port, net::Resolver* resolver,
                         tls::TlsContextPtr tls_ctx, std::string host_header,
                         Options options)
    : impl_(std::make_shared<Impl>(loop, host, port, resolver,
                                   std::move(tls_ctx), std::move(host_header),
                                   options)) {}

size_t Http2Client::openConnections() const {
    return impl_->openConnections();
}

Http2Client::~Http2Client() = default;

void Http2Client::send(const HttpRequest& req, ResponseCallback cb) {
    impl_->send(req, std::move(cb), 0);
}

void Http2Client::get(const std::string& path, ResponseCallback cb) {
    HttpRequest req;
    req.setMethod(Method::kGet);
    const auto q = path.find('?');
    req.setPath(path.substr(0, q));
    if (q != std::string::npos) {
        req.setQuery(path.substr(q + 1));
    }
    send(req, std::move(cb));
}

void Http2Client::shutdown() {
    impl_->shutdown();
}

uint64_t Http2Client::connectionsOpened() const {
    return impl_->opened();
}

uint64_t Http2Client::retries() const {
    return impl_->retries();
}

}  // namespace http
