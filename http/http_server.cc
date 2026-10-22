#include "http_server.h"

#include "buffer.h"
#include "event_loop.h"
#include "http_context.h"
#include "logger.h"
#include "tcp_connection.h"
#include "tls_connection.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace http
{
namespace
{
logging::Logger& log()
{
    static logging::Logger logger;
    return logger;
}
} // namespace

// Per-connection state. For TLS, holds the TlsConnection and a reassembly
// buffer for decrypted plaintext; always holds the HTTP parse context.
struct HttpServer::PerConn
{
    HttpContext ctx;
    tls::TlsConnectionPtr tls;              // Null for plaintext
    buffer::Buffer in;                      // reassemebled plaintext (TLS path)
};

HttpServer::HttpServer(reactor::EventLoop* loop, const net::InetAddress& addr, 
                        std::string name)
    : server_(loop, addr, std::move(name))
{
    server_.setConnectionCallback(
        [this](const net::TcpConnectionPtr& c) { 
            onConnection(c); 
        });

    server_.setMessageCallback(
        [this](const net::TcpConnectionPtr& c, buffer::Buffer* b) {
            onPlainMessage(c, b);
        });

    server_.setIdleTimeout(60);
}

HttpServer::HttpServer(reactor::EventLoop* loop, const net::InetAddress& addr, 
                        std::string name, tls::TlsContextPtr tls_ctx)
    : tls_ctx_(std::move(tls_ctx)), server_(loop, addr, std::move(name))
{
    server_.setConnectionCallback(
        [this](const net::TcpConnectionPtr& c) { 
            onConnection(c); 
        });

    server_.setMessageCallback(
        [this](const net::TcpConnectionPtr& c, buffer::Buffer* b) {
            onPlainMessage(c, b);
        });

    server_.setIdleTimeout(60);
}

HttpServer::~HttpServer() = default;

void HttpServer::start() 
{ 
    server_.start(); 
}

void HttpServer::onConnection(const net::TcpConnectionPtr& conn)
{
    if (conn->connected())
    {
        auto pc = std::make_shared<PerConn>();
        if (tls_ctx_)
        {
            // Wrap in server-side TLS; feed decrypted plaintext to the parser.
            auto tc = std::make_shared<tls::TlsConnection>(
                conn, tls_ctx_, tls::Role::kServer);
            pc->tls = tc;
            std::string name = conn->name();
            tc->setMessageCallback([this, name](const tls::TlsConnectionPtr&, std::string& pt){
                // Route decrypted bytes; find state by connection name.
                auto it = conns_.find(name);
                if(it == conns_.end()) return;
                auto pcp = it->second;
                pcp->in.append(pt);
                auto self_tls = pcp->tls;
                processInput(name, &pcp->in, 
                            [self_tls](const std::string& data){
                                self_tls->send(data);
                            }, [self_tls](){self_tls->shutdown(); });
            });
            conns_[conn->name()] = pc;
        }
        else
        {
            conns_[conn->name()] = pc;
        }
    }
    else
    {
        conns_.erase(conn->name());
    }
    if (connection_observer_) connection_observer_(conn);
}

void HttpServer::onPlainMessage(const net::TcpConnectionPtr& conn,
                                buffer::Buffer* buf)
{
    if (tls_ctx_) return;           // TLS path handles bytes via TlsConnection
    std::string name = conn->name();
    net::TcpConnectionPtr c = conn;
    processInput(
        name, buf,
        [c](const std::string& data) {c->send(data); },
        [c](){ c->shutdown(); });
}

void HttpServer::processInput(
    const std::string& conn_name, buffer::Buffer* in,
    const std::function<void(const std::string&)>& send,
    const std::function<void()>& close )
{
    auto it = conns_.find(conn_name);
    if (it == conns_.end()) return;

    HttpContext& ctx = it->second->ctx;

    if (!ctx.parseRequest(in))
    {
        log().debug("malformed request on " + conn_name + "; sending 400");
        send("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
             "Connection: close\r\n\r\n");
        close();
        return;
    }

    while(ctx.gotAll())
    {
        const HttpRequest& req = ctx.request();
        const bool keep_alive = req.keepAlive();

        HttpResponse resp(keep_alive);
        if (handler_)
        {
            handler_(req, &resp);
        }
        else
        {
            resp.setStatus(404);
            resp.setBody("No handler");
        }
        resp.setKeepAlive(keep_alive);

        buffer::Buffer out;
        resp.appendTo(&out);
        send(out.retrieveAllAsString());

        ctx.reset();
        if (!keep_alive)
        {
            close();
            break;
        }

        // Parse a possibly-pipelined next request already buffered.
        if (!ctx.parseRequest(in))
        {
            log().debug("malformed pipelined request on " + conn_name +
                         "; sending 400");
            send("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                    "Connection: close\r\n\r\n");
            close();
            return;
        }
    }
}

} // namespace http