#include "http_server.h"

#include "buffer.h"
#include "event_loop.h"
#include "http_context.h"
#include "logger.h"
#include "tcp_connection.h"
#include "tls_connection.h"

#ifdef COX_WITH_HTTP2
#include "http2_session.h"
#endif

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
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

// Per-connection state. For TLS, holds the TlsConnection and a reassembly
// buffer for decrypted plaintext; always holds the HTTP parse context.
struct HttpServer::PerConn {
    HttpContext ctx;
    tls::TlsConnectionPtr tls;  // null for plaintext
    buffer::Buffer in;          // reassembled plaintext (TLS path)
#ifdef COX_WITH_HTTP2
    // Set when ALPN selected "h2"; then the HTTP/1.1 parser is unused.
    std::unique_ptr<Http2Session> h2;
    std::string name;
    bool closing{false};             // TLS shutdown already initiated
    bool goaway_timer_armed{false};  // step-2 / grace timers scheduled
#endif
};

namespace {
#ifdef COX_WITH_HTTP2
Http2Session::Limits toLimits(const HttpServer::Http2Options& o,
                              size_t max_body) {
    Http2Session::Limits l;
    l.max_concurrent_streams = o.max_concurrent_streams;
    l.max_header_list_bytes = o.max_header_list_bytes;
    l.initial_window_size = o.initial_window_size;
    l.connection_window_size = o.connection_window_size;
    l.max_frame_size = o.max_frame_size;
    l.max_stream_id = o.max_stream_id;
    l.max_requests = o.max_requests_per_connection;
    l.max_body_bytes = max_body;
    return l;
}
#endif
}  // namespace

void HttpServer::setHttp2Options(const Http2Options& opts) {
#ifdef COX_WITH_HTTP2
    std::string err = Http2Session::validate(toLimits(opts, 0));
    if (!err.empty()) {
        throw std::invalid_argument("HttpServer::setHttp2Options: " + err);
    }
#endif
    h2_opts_ = opts;
}

size_t HttpServer::http2Connections() const {
    size_t n = 0;
#ifdef COX_WITH_HTTP2
    for (const auto& [name, pc] : conns_) {
        n += pc->h2 != nullptr;
    }
#endif
    return n;
}

void HttpServer::goAwayHttp2Connections() {
#ifdef COX_WITH_HTTP2
    std::vector<std::string> names;
    for (const auto& [name, pc] : conns_) {
        if (pc->h2) {
            names.push_back(name);
        }
    }
    for (const auto& name : names) {
        goAwayHttp2(name, /*immdiate=*/false);
    }
#endif
}

HttpServer::HttpServer(reactor::EventLoop* loop, const net::InetAddress& addr,
                       std::string name)
    : server_(loop, addr, std::move(name)) {
    server_.setConnectionCallback(
        [this](const net::TcpConnectionPtr& c) { onConnection(c); });

    server_.setMessageCallback(
        [this](const net::TcpConnectionPtr& c, buffer::Buffer* b) {
            onPlainMessage(c, b);
        });

    server_.setIdleTimeout(60);
}

HttpServer::HttpServer(reactor::EventLoop* loop, const net::InetAddress& addr,
                       std::string name, tls::TlsContextPtr tls_ctx)
    : tls_ctx_(std::move(tls_ctx)), server_(loop, addr, std::move(name)) {
    server_.setConnectionCallback(
        [this](const net::TcpConnectionPtr& c) { onConnection(c); });

    server_.setMessageCallback(
        [this](const net::TcpConnectionPtr& c, buffer::Buffer* b) {
            onPlainMessage(c, b);
        });

    server_.setIdleTimeout(60);
#ifdef COX_WITH_HTTP2
    // Server drain: h2 connections GOAWAY and finish in-flight streams
    // instead of being half-closed under them.
    server_.setDrainTakeoverCallback([this](const net::TcpConnectionPtr& c) {
        auto it = conns_.find(c->name());
        if (it == conns_.end() || !it->second->h2) {
            return false;
        }
        goAwayHttp2(c->name(), /*immediate=*/false);
        return true;
    });
#endif
}

HttpServer::~HttpServer() = default;

void HttpServer::start() {
    server_.start();
}

void HttpServer::onConnection(const net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        auto pc = std::make_shared<PerConn>();
        pc->ctx.setMaxBodyBytes(max_body_bytes_);
        if (tls_ctx_) {
            // Wrap in server-side TLS; feed decrypted plaintext to the parser.
            auto tc = std::make_shared<tls::TlsConnection>(conn, tls_ctx_,
                                                           tls::Role::kServer);
            pc->tls = tc;
            std::string name = conn->name();
#ifdef COX_WITH_HTTP2
            // ALPN is known once the handshake completes: switch this
            // connection to HTTP/2 if "h2" was agreed.
            tc->setHandshakeDoneCallback(
                [this, name](const tls::TlsConnectionPtr& t) {
                    if (t->negotiatedProtocol() != "h2") {
                        return;  // http/1.1 or no ALPN: keep the h1 parser
                    }
                    auto it = conns_.find(name);
                    if (it != conns_.end()) {
                        startHttp2(*it->second);
                    }
                });
            // Resume pulling h2 frames when the socket's output drains, and
            // close once a GOAWAY'd session has finished.
            conn->setWriteCompleteCallback(
                [this, name](const net::TcpConnectionPtr&) {
                    auto it = conns_.find(name);
                    if (it != conns_.end() && it->second->h2) {
                        it->second->h2->flush();
                        maybeCloseHttp2(*it->second);
                    }
                });
            // Max connection age: GOAWAY instead of a hard close for h2.
            conn->setMaxAgeCallback(
                [this, name](const net::TcpConnectionPtr&) {
                    auto it = conns_.find(name);
                    if (it == conns_.end() || !it->second->h2) {
                        return false;  // HTTP/1.1: default force-close
                    }
                    goAwayHttp2(name, /*immediate=*/false);
                    return true;
                },
                h2_opts_.goaway_grace_seconds +
                    (h2_opts_.goaway_notice_ms + 999) / 1000);
#endif
            tc->setMessageCallback([this, name](const tls::TlsConnectionPtr&,
                                                std::string& pt) {
                // Route decrypted bytes; find state by connection name.
                auto it = conns_.find(name);
                if (it == conns_.end()) {
                    return;
                }
                auto pcp = it->second;
#ifdef COX_WITH_HTTP2
                if (pcp->h2) {
                    if (!pcp->h2->feed(pt.data(), pt.size())) {
                        log().debug("closing h2 connection " + name + ": " +
                                    pcp->h2->lastError());
                        pcp->closing = true;
                        pcp->tls->shutdown();
                        return;
                    }
                    if (pcp->h2->goingAway() && !pcp->h2->finishGoAwaySent()) {
                        // A stream-ID / request limit sent the notice:
                        // schedule the final GOAWAY.
                        goAwayHttp2(name, /*immediate=*/false);
                    }
                    maybeCloseHttp2(*pcp);
                    return;
                }
#endif
                pcp->in.append(pt);
                auto self_tls = pcp->tls;
                processInput(
                    name, &pcp->in,
                    [self_tls](const std::string& data) {
                        self_tls->send(data);
                    },
                    [self_tls]() { self_tls->shutdown(); });
            });
            conns_[conn->name()] = pc;
            tc->start();
        } else {
            conns_[conn->name()] = pc;
        }
    } else {
        conns_.erase(conn->name());
    }
    if (connection_observer_) {
        connection_observer_(conn);
    }
}

void HttpServer::onPlainMessage(const net::TcpConnectionPtr& conn,
                                buffer::Buffer* buf) {
    if (tls_ctx_) {
        return;  // TLS path handles bytes via TlsConnection
    }
    std::string name = conn->name();
    net::TcpConnectionPtr c = conn;
    processInput(
        name, buf, [c](const std::string& data) { c->send(data); },
        [c]() { c->shutdown(); });
}

#ifdef COX_WITH_HTTP2
void HttpServer::startHttp2(PerConn& pc) {
    // Raw pointers into PerConn/TlsConnection are safe in these callbacks:
    // the Http2Session is owned by the same PerConn and is destroyed with it,
    // so it never calls back after the connection state is gone.
    tls::TlsConnection* tls = pc.tls.get();
    net::TcpConnection* tcp = tls->tcpConnection().get();

    Http2Session::Callbacks cbs;
    cbs.handler = handler_;
    cbs.stream_handler = stream_handler_;
    cbs.loop = tcp->getLoop();
    cbs.send = [tls](const char* data, size_t len) {
        tls->send(std::string(data, len));
    };
    cbs.pending = [tcp] { return tcp->outputBufferPending(); };
    cbs.on_request = [this](int status, double secs) {
        if (request_observer_) {
            request_observer_(status, secs);
        }
    };
    cbs.on_reject = [this](int status) {
        if (status == 413) {
            rejected_too_large_.fetch_add(1, std::memory_order_relaxed);
        } else {
            rejected_bad_request_.fetch_add(1, std::memory_order_relaxed);
        }
    };

    Http2Session::Limits limits = toLimits(h2_opts_, max_body_bytes_);
    pc.name = tcp->name();
    pc.h2 = std::make_unique<Http2Session>(std::move(cbs), limits);
    if (!pc.h2->start()) {
        log().error("HTTP/2 session init failed: " + pc.h2->lastError());
        pc.closing = true;
        pc.tls->shutdown();
    }
}

void HttpServer::goAwayHttp2(const std::string& name, bool immediate) {
    auto it = conns_.find(name);
    if (it == conns_.end() || !it->second->h2 || it->second->closing) {
        return;
    }
    PerConn& pc = *it->second;
    // Timers go onn the connection's own io loop (with setThreadNum(N) it is
    // not the server's base loop), the same thread that touches this state.
    reactor::EventLoop* loop = pc.tls->tcpConnection()->getLoop();
    if (!pc.h2->goingAway()) {
        pc.h2->shutdown(immediate);  // step 1: notice (or final if immediate)
    }
    // Arm the step-2 and grace timers once per connection, however many
    // timers this is called while the notice is pending.
    if (!pc.goaway_timer_armed) {
        pc.goaway_timer_armed = true;
        if (!pc.h2->finalGoAwaySent()) {
            // Step 2 after the notice delay. Look the connection up again
            // by name: it may have closed in the meantime.
            loop->runAfter(h2_opts_.goaway_notice_ms, [this, name]() {
                auto i = conns_.find(name);
                if (i == conns_.end() || !i->second->h2) {
                    return;
                }
                i->second->h2->shutdown();  // final GOAWAY
                maybeCloseHttp2(*i->second);
            });
        }
        if (h2_opts_.goaway_grace_seconds > 0) {
            const uint64_t ms = h2_opts_.goaway_notice_ms +
                                uint64_t{h2_opts_.goaway_grace_seconds} * 1000;
            std::weak_ptr<net::TcpConnection> weak = pc.tls->tcpConnection();
            loop->runAfter(ms, [weak]() {
                if (auto c = weak.lock()) {
                    c->forceClose();  // stream outlived the grace period
                }
            });
        }
    }
    maybeCloseHttp2(pc);
}

void HttpServer::maybeCloseHttp2(PerConn& pc) {
    if (pc.closing || !pc.h2 || !pc.h2->finished()) {
        return;
    }
    log().debug("h2 connection " + pc.name + " finished; closing");
    pc.closing = true;
    pc.tls->shutdown();
}
#endif

namespace {
// Collects a ServerStream's response into an HttpResponse, for transports
// without async streaming (HTTP/1.1 in phase 1).
class CollectSink : public ServerStream::Sink {
public:
    explicit CollectSink(reactor::EventLoop* loop) : loop_(loop) {}
    void sendHead(int status, const ServerStream::Headers& headers, bool,
                  bool chunked) override {
        resp.setStatus(status);
        for (const auto& [k, v] : headers) {
            std::string lk = k;
            for (auto& c : lk) {
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            }
            if (lk != "content-length") {  // recomputed on serialize
                resp.addHeader(k, v);
            }
        }
        streamed = chunked;
        head = true;
    }
    size_t sendData(std::string_view d) override {
        body.append(d.data(), d.size());
        return 0;
    }
    void sendEnd() override { ended = true; }
    void abandoned() override {}
    reactor::EventLoop* loop() const override { return loop_; }

    HttpResponse resp;
    std::string body;
    bool head{false};
    bool ended{false};
    bool streamed{false};

private:
    reactor::EventLoop* loop_;
};
}  // namespace

HttpResponse HttpServer::runStreamHandlerSync(const HttpRequest& req,
                                              bool keep_alive) {
    auto sink = std::make_shared<CollectSink>(server_.getLoop());
    {
        auto stream = std::make_shared<ServerStream>(req, sink, SIZE_MAX);
        stream_handler_(stream);
        if (!sink->ended) {
            // Still pending after the handler returned (or dropped): not
            // supported on HTTP/1.1 yet. Abort it so the handler stops.
            stream->notifyAbort();
        }
    }
    HttpResponse out(keep_alive);
    if (!sink->ended) {
        log().warning(
            "stream handler did not finish an HTTP/1.1 request before "
            "returning; async/streamed responses are HTTP/2-only for now");
        out.setStatus(500);
        return out;
    }
    out = sink->resp;
    out.setKeepAlive(keep_alive);
    out.setBody(std::move(sink->body));
    return out;
}

void HttpServer::processInput(
    const std::string& conn_name, buffer::Buffer* in,
    const std::function<void(const std::string&)>& send,
    const std::function<void()>& close) {
    auto it = conns_.find(conn_name);
    if (it == conns_.end()) {
        return;
    }

    HttpContext& ctx = it->second->ctx;

    // Notify the request observer (metrics/tracing hook). `status` is the sent
    // HTTP status; `secs` is handler wall time (0 for parser-rejected paths).
    auto notify = [this](int status, double secs) {
        if (request_observer_) {
            request_observer_(status, secs);
        }
    };

    // Count a parser rejection by reason (feeds cox_http_rejected_total).
    auto countRejection = [this](int status) {
        if (status == 413) {
            rejected_too_large_.fetch_add(1, std::memory_order_relaxed);
        } else {
            rejected_bad_request_.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // Build the error response matching the parser's failure reason: an
    // oversized body is 413 Payload Too Large; anything else malformed is 400.
    auto errorResponse = [](HttpContext::Error e) -> std::string {
        if (e == HttpContext::Error::kTooLarge) {
            return "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\n"
                   "Connection: close\r\n\r\n";
        }
        return "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
               "Connection: close\r\n\r\n";
    };

    if (!ctx.parseRequest(in)) {
        int status = ctx.error() == HttpContext::Error::kTooLarge ? 413 : 400;
        log().debug("rejected request on " + conn_name + " (" +
                    (status == 413 ? "body too large; 413" : "malformed; 400") +
                    ")");
        send(errorResponse(ctx.error()));
        countRejection(status);
        notify(status, 0.0);
        close();
        return;
    }

    while (ctx.gotAll()) {
        const HttpRequest& req = ctx.request();
        const bool keep_alive = req.keepAlive();

        HttpResponse resp(keep_alive);
        auto t0 = std::chrono::steady_clock::now();
        if (handler_) {
            handler_(req, &resp);
        } else if (stream_handler_) {
            // Phase 1: HTTP/1.1 has no async/streaming transport yet, so the
            // stream handler must answer before returning; a buffered
            // response is collected and sent. A deferred or streamed answer
            // gets 500 here (phase 2 lifts this; see
            // docs/insights/features/http-streaming-design.md).
            resp = runStreamHandlerSync(req, keep_alive);
        } else {
            resp.setStatus(404);
            resp.setBody("No handler");
        }
        double handler_secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                .count();
        notify(resp.statusCode(), handler_secs);
        resp.setKeepAlive(keep_alive);

        buffer::Buffer out;
        resp.appendTo(&out);
        send(out.retrieveAllAsString());

        ctx.reset();
        if (!keep_alive) {
            close();
            break;
        }

        // Parse a possibly-pipelined next request already buffered.
        if (!ctx.parseRequest(in)) {
            int status =
                ctx.error() == HttpContext::Error::kTooLarge ? 413 : 400;
            log().debug(
                "rejected pipelined request on " + conn_name + " (" +
                (status == 413 ? "body too large; 413" : "malformed; 400") +
                ")");
            send(errorResponse(ctx.error()));
            countRejection(status);
            notify(status, 0.0);
            close();
            return;
        }
    }
}

}  // namespace http
