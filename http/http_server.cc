#include "http_server.h"

#include "buffer.h"
#include "event_loop.h"
#include "http_context.h"
#include "logger.h"
#include "tcp_connection.h"
#include "tls_connection.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

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
};

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
}

HttpServer::~HttpServer() = default;

void HttpServer::start() {
    server_.start();
}

void HttpServer::onConnection(const net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        auto pc = std::make_shared<PerConn>();
        if (tls_ctx_) {
            // Wrap in server-side TLS; feed decrypted plaintext to the parser.
            auto tc = std::make_shared<tls::TlsConnection>(conn, tls_ctx_,
                                                           tls::Role::kServer);
            pc->tls = tc;
            std::string name = conn->name();
            tc->setMessageCallback(
                [this, name](const tls::TlsConnectionPtr&, std::string& pt) {
                    // Route decrypted bytes; find state by connection name.
                    auto it = conns_.find(name);
                    if (it == conns_.end()) {
                        return;
                    }
                    auto pcp = it->second;
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

    // Build the error response matching the parser's failure reason: an
    // oversized body is 413 payload Too Large; anything else malformed is 400.
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
            notify(status, 0.0);
            close();
            return;
        }
    }
}

}  // namespace http
