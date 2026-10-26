#include "http_client.h"

#include "connection_pool.h"

#include <memory>
#include <string>
#include <utility>

namespace http
{
// HttpClient is a thin facade over a single-connection ConnectionPool. This
// reuses the pool's keep-alive + TLS + parsing machinery instead of
// duplicating it, and keeps one code path for the subtle reuse/TLS logic.

HttpClient::HttpClient(reactor::EventLoop* loop, const net::InetAddress& server,
                        std::string host_header)
    : pool_(std::make_shared<ConnectionPool>(loop, server, std::move(host_header)))
{
    pool_->setMaxConnections(1);            // Single connection, resued (keep-alive)
}

HttpClient::HttpClient(reactor::EventLoop* loop, const net::InetAddress& server,
                        tls::TlsContextPtr tls_ctx, std::string host_header)
    : pool_(std::make_shared<ConnectionPool>(loop, server, std::move(tls_ctx),
                                            std::move(host_header)))
{
    pool_->setMaxConnections(1);
}

HttpClient::HttpClient(reactor::EventLoop* loop, const std::string& host,
                        uint16_t port, net::Resolver* resolver,
                        std::string host_header)
    : pool_(std::make_shared<ConnectionPool>(loop, host, port, resolver,
                                             std::move(host_header)))
{
    pool_->setMaxConnections(1);
}

HttpClient::HttpClient(reactor::EventLoop* loop, const std::string& host,
                        uint16_t port, net::Resolver* resolver,
                        tls::TlsContextPtr tls_ctx, std::string host_header)
    : pool_(std::make_shared<ConnectionPool>(loop, host, port, resolver,
                                             std::move(tls_ctx),
                                             std::move(host_header)))
{
    pool_->setMaxConnections(1);
}

HttpClient::~HttpClient() = default;

void HttpClient::send(const HttpRequest& req, ResponseCallback cb)
{
    pool_->request(req, std::move(cb));
}

void HttpClient::get(const std::string& path, ResponseCallback cb)
{
    HttpRequest req;
    req.setMethod(Method::kGet);
    req.setPath(path);
    send(req, std::move(cb));
}

} // namespace http