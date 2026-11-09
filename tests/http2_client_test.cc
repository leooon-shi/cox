// End-to-end tests for http::Http2Client against a real Cox HTTP/2 server
// over TLS. Only built with COX_WITH_HTTP2. OPAQUE: shells out to openssl.

#include "http2_client.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"

#include "event_loop.h"
#include "inet_address.h"
#include "resolver.h"
#include "thread_pool.h"
#include "tls_context.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct TestCert {
    std::string cert = "/tmp/cox_h2c_cert.pem";
    std::string key = "/tmp/cox_h2c_key.pem";
    TestCert() {
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout " +
                          key + " -out " + cert +
                          " -days 1 -subj '/CN=localhost' >/dev/null 2>&1";
        (void)test_util::runCmd(cmd);
    }
    ~TestCert() {
        std::remove(cert.c_str());
        std::remove(key.c_str());
    }

    bool valid() const {
        std::ifstream c(cert), k(key);
        return c.good() && k.good();
    }
};

// HTTPS server on its own loop thread.
class Server {
public:
    Server(uint16_t port, const TestCert& cert,
           http::HttpServer::Http2Options h2 = {},
           std::vector<std::string> alpn = {"h2", "http/1.1"}) {
        thread_ = std::thread([this, port, &cert, h2, alpn] {
            reactor::EventLoop loop;
            loop_ = &1oop;
            auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kServer);
            ctx->useCertificateAndKey(cert.cert, cert.key);
            ctx->setAlpnProtocols(alpn);
            http::HttpServer server(&loop, net::InetAddress("127.0.0.1", port),
                                    "h2s", ctx);
            server.setHttp2Options(h2);
            server.setConnectionObserver(
                [this](const net::TcpConnectionPtr& c) {
                    if (c->connected()) {
                        connections_++;
                    }
                });
            server.setHttpHandler(
                [this](const http::HttpRequest& req, http::HttpResponse* resp) {
                    handled_++;
                    resp->setStatus(200);
                    resp->setBody(req.version() + " " + req.path() + " " +
                                  req.body());
                });
            server.start();
            started_ = true;
            loop.loop();
        });
        while (!started_.load()) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(30ms);
    }
    ~Server() {
        loop_->runInLoop([this] { loop_->quit(); });
        thread_.join();
    }
    std::atomic<int> handled_{0};
    std::atomic<int> connections_{0};

private:
    std::thread thread_;
    std::atomic<bool> started_{false};
    reactor::EventLoop* loop_{nullptr};
};

struct Outcome {
    size_t max_open = 0;  // most connections open at once
    int ok = 0;
    int failed = 0;
    std::set<std::string> bodies;
    std::vector<std::string> errors;
};

// Run `n` requests through one Http2Client on a fresh loop and collect the
// results. `stagger_ms` > 0 sends them one per interval instead of at once.
using ClientFactory = std::function<std::unique_ptr<http::Http2Client>(
    reactor::EventLoop*, tls::TlsContextPtr)>;

Outcome runRequests(uint16_t port, int n, http::Http2Client::Options opts,
                    uint64_t* connections_opened, uint64_t* retries,
                    int stagger_ms = 0, ClientFactory make = nullptr) {
    Outcome out;
    reactor::EventLoop loop;
    auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kClient);
    ctx->setVerifyPeer(false);  // self-signed test cert
    std::unique_ptr<http::Http2Client> owned =
        make ? make(&loop, ctx)
             : std::make_unique<http::Http2Client>(
                   &loop, net::InetAddress("127.0.0.1", port), ctx, "localhost",
                   opts);
    http::Http2Client& client = *owned;
    int done = 0;
    auto finish = [&](const http::HttpResponse& resp, const std::string& err) {
        if (err.empty() && resp.statusCode() == 200) {
            out.ok++;
            out.bodies.insert(resp.body());
        } else {
            out.failed++;
            out.errors.push_back(err);
        }
        out.max_open = std::max(out.max_open, client.openConnections());
        if (++done == n) {
            loop.quit();
        }
    };
    for (int i = 0; i < n; ++i) {
        std::string path = "/r" + std::to_string(i);
        if (stagger_ms > 0) {
            loop.runAfter(
                static_cast<uint64_t>(i * stagger_ms),
                [&client, path, finish] { client.get(path, finish); });
        } else {
            client.get(path, finish);
        }
    }
    loop.runAfter(15000, [&] { loop.quit(); });  // safety net
    loop.loop();
    out.max_open = std::max(out.max_open, client.openConnections());
    *connections_opened = client.connectionsOpened();
    *retries = client.retries();
    return out;
}
}  // namespace

TEST(Http2ClientTest, MultiplexesConcurrentRequestsOnOneConnection) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56301;
    Server server(kPort, cert);

    uint64_t opened = 0;
    uint64_t retries = 0;
    Outcome o = runRequests(kPort, 50, {}, &opened, &retries);
    EXPECT_EQ(o.ok, 50);
    EXPECT_EQ(o.failed, 0);
    EXPECT_EQ(o.bodies.size(), 50u);  // every response distinct and correct
    EXPECT_TRUE(o.bodies.count("HTTP/2 /r7 "));
    EXPECT_EQ(opened, 1u) << "all 50 requests share one connection";
    EXPECT_EQ(server.connections_.load(), 1);
    EXPECT_EQ(retries, 0u);
}

TEST(Http2ClientTest, ReconnectsAfterServerGoAwayFromRequestCap) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56302;
    http::HttpServer::Http2Options h2;
    h2.max_requests_per_connection = 5;
    h2.goaway_notice_ms = 20;
    Server server(kPort, cert, h2);

    uint64_t opened = 0;
    uint64_t retries = 0;
    // Staggered so GOAWAYs land between requests: the client must move to a
    // new connection each time the server's cap is hit.
    Outcome o = runRequests(kPort, 20, {}, &opened, &retries,
                            /*stagger_ms=*/30);
    EXPECT_EQ(o.ok, 20) << (o.errors.empty() ? "" : o.errors.front());
    EXPECT_EQ(o.failed, 0);
    EXPECT_GE(opened, 4u) << "a cap of 5 requests needs >= 4 connections";
    EXPECT_EQ(server.handled_.load(), 20) << "no request processed twice";
}

TEST(Http2ClientTest, BurstAcrossGoAwayIsRetriedNotLost) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56303;
    http::HttpServer::Http2Options h2;
    h2.max_requests_per_connection = 10;
    h2.goaway_notice_ms = 20;
    Server server(kPort, cert, h2);

    uint64_t opened = 0;
    uint64_t retries = 0;
    // All 40 at once: many streams are already in flight when the server
    // decides to GOAWAY. Whatever the server did not process must be re-sent
    // on a new connection; none may be lost or processed twice.
    //
    // How many connections that takes depends on timing and is not asserted:
    // when all 40 HEADERS arrive in one read, the server answers every one
    // of them before its GOAWAY notice is even sent (they are all at or below
    // the final last_stream_id), so one connection is the CORRECT outcome.
    // (An earlier version expected >= 2 connections; that only held because
    // a bug sent the final GOAWAY too early, which also duplicated requests.)
    Outcome o = runRequests(kPort, 40, {}, &opened, &retries);
    EXPECT_EQ(o.ok, 40) << (o.errors.empty() ? "" : o.errors.front());
    EXPECT_EQ(o.bodies.size(), 40u);
    EXPECT_GE(opened, 1u);
    EXPECT_EQ(server.handled_.load(), 40) << "a retry must never duplicate";
}

TEST(Http2ClientTest, InFlightAcrossGoAwayRetriedOnNewConnection) {
    // Deterministic version of the burst: the server caps at 2 concurrent
    // streams and 3 requests per connection, and the client sends 12 at
    // once. Streams queue locally behind the stream cap, so the server's
    // GOAWAY always arrives with requests still waiting on the old
    // connection; they must move to new connections, none lost, none run
    // twice.
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56313;
    http::HttpServer::Http2Options h2;
    h2.max_concurrent_streams = 2;
    h2.max_requests_per_connection = 3;
    h2.goaway_notice_ms = 20;
    Server server(kPort, cert, h2);

    http::Http2Client::Options opts;
    opts.max_connections = 1;  // force reuse until GOAWAY, then rotate
    opts.max_retries = 10;
    uint64_t opened = 0;
    uint64_t retries = 0;
    Outcome o = runRequests(kPort, 12, opts, &opened, &retries);
    EXPECT_EQ(o.ok, 12) << (o.errors.empty() ? "" : o.errors.front());
    EXPECT_EQ(o.bodies.size(), 12u);
    // The cap starts the GOAWAY after the 3rd request, but a request already
    // in flight (stream limit 2, so at most one more) is below the final
    // last_stream_id and is still served. So a connection carries at most
    // 3 + 1 = 4 requests, and 12 requests need at least 3 connections.
    EXPECT_GE(opened, 3u);
    EXPECT_GE(retries, 1u) << "requests queued on a GOAWAY'd connection";
    EXPECT_EQ(server.handled_.load(), 12) << "no request processed twice";
}

TEST(Http2ClientTest, MovesToNewConnectionWhenStreamIdsRunOut) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56304;
    Server server(kPort, cert);

    http::Http2Client::Options opts;
    opts.max_stream_id = 7;  // 4 requests per connection (streams 1,3,5,7)
    uint64_t opened = 0;
    uint64_t retries = 0;
    Outcome o = runRequests(kPort, 12, opts, &opened,
                            &retries
                            /*stagger_ms=*/10);
    EXPECT_EQ(o.ok, 12) << (o.errors.empty() ? "" : o.errors.front());
    EXPECT_EQ(opened, 3u) << "12 requests / 4 stream IDs per connection";
    EXPECT_EQ(server.handled_.load(), 12);
}

TEST(Http2ClientTest, ServerWithoutH2FailsClearly) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56305;
    Server server(kPort, cert, {}, {"http/1.1"});

    uint64_t opened = 0;
    uint64_t retries = 0;
    Outcome o = runRequests(kPort, 3, {}, &opened, &retries);
    EXPECT_EQ(o.ok, 0);
    EXPECT_EQ(o.failed, 3);
    EXPECT_FALSE(o.errors.empty());
    EXPECT_NE(o.errors.front().find("does not support HTTP/2"),
              std::string::npos)
        << o.errors.front();
    EXPECT_EQ(server.handled_.load(), 0);
}

TEST(Http2ClientTest, UnreachableServerFailsAllRequests) {
    // Nothing listens here; requests must fail rather than hang.
    const uint16_t kPort = 56306;
    http::Http2Client::Options opts;
    opts.connect_timeout_ms = 500;
    uint64_t opened = 0;
    uint64_t retries = 0;
    const auto t0 = std::chrono::steady_clock::now();
    Outcome o = runRequests(kPort, 3, opts, &opened, &retries);
    const auto took = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(o.ok, 0);
    EXPECT_EQ(o.failed, 3);
    ASSERT_FALSE(o.errors.empty());
    EXPECT_EQ(o.errors.front(), "connect timed out");
    EXPECT_LT(took, 5s) << "requests must fail at the connect timeout";
}

// ---- Multiple connections per origin ----

TEST(Http2ClientTest, OpensMoreConnectionsWhenStreamLimitIsReached) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56307;
    http::HttpServer::Http2Options h2;
    h2.max_concurrent_streams = 2;  // each connection carries 2 streams
    Server server(kPort, cert, h2);

    http::Http2Client::Options opts;
    opts.max_connections = 3;
    uint64_t opened = 0;
    uint64_t retries = 0;
    // 30 requests at once. One connection would queue 28 of them; the pool
    // spreads them over up to 3 connections, never more.
    Outcome o = runRequests(kPort, 30, opts, &opened, &retries);
    EXPECT_EQ(o.ok, 30) << (o.errors.empty() ? "" : o.errors.front());
    EXPECT_EQ(o.bodies.size(), 30u);
    EXPECT_GE(opened, 2u) << "a full connection must trigger a new one";
    EXPECT_LE(o.max_open, 3u) << "never more than max_connections";
    EXPECT_EQ(server.handled_.load(), 30);
}

TEST(Http2ClientTest, MaxConnectionsOneKeepsASingleConnection) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56308;
    http::HttpServer::Http2Options h2;
    h2.max_concurrent_streams = 2;
    Server server(kPort, cert, h2);

    http::Http2Client::Options opts;
    opts.max_connections = 1;
    uint64_t opened = 0;
    uint64_t retries = 0;
    Outcome o = runRequests(kPort, 20, opts, &opened, &retries);
    EXPECT_EQ(o.ok, 20) << (o.errors.empty() ? "" : o.errors.front());
    EXPECT_EQ(opened, 1u) << "requests queue instead of opening more";
    EXPECT_EQ(server.connections_.load(), 1);
}

TEST(Http2ClientTest, LightLoadStaysOnOneConnection) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56309;
    Server server(kPort, cert);  // default 100 streams per connection

    uint64_t opened = 0;
    uint64_t retries = 0;
    // 50 concurrent requests fit in one connection's stream limit, so the
    // pool (default max_connections 4) must not open a second one.
    Outcome o = runRequests(kPort, 50, {}, &opened, &retries);
    EXPECT_EQ(o.ok, 50);
    EXPECT_EQ(opened, 1u);
}

// ---- Hostname origins (DNS) ----

namespace {
// Resolver double: answers from a table, counts lookups, can fail.
class FakeResolver : public net::Resolver {
public:
    explicit FakeResolver(reactor::EventLoop* loop) : loop_(loop) {}
    std::vector<net::InetAddress> answer;  // empty -> failure
    int lookups = 0;

    void resolve(const std::string& host, uint16_t port, Callback cb) override {
        resolveAll(host, port,
                   [cb](const std::vector<net::InetAddress>& a,
                        const std::string& err) {
                       cb(a.empty() ? net::InetAddress() : a.front(), err);
                   });
    }
    void resolveAll(const std::string&, uint16_t, ListCallback cb) override {
        ++lookups;
        auto a = answer;
        loop_->queueInLoop([cb, a] {  // async, like the real resolvers
            cb(a, a.empty() ? "no such host" : "");
        });
    }

private:
    reactor::EventLoop* loop_;
};
}  // namespace

TEST(Http2ClientTest, HostnameIsResolvedOnceAndReused) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56310;
    Server server(kPort, cert);

    FakeResolver* resolver = nullptr;
    std::unique_ptr<FakeResolver> owned_resolver;
    uint64_t opened = 0;
    uint64_t retries = 0;
    Outcome o = runRequests(
        kPort, 10, {}, &opened, &retries, 0,
        [&](reactor::EventLoop* loop, tls::TlsContextPtr ctx) {
            owned_resolver = std::make_unique<FakeResolver>(loop);
            resolver = owned_resolver.get();
            resolver->answer = {net::InetAddress("127.0.0.1", kPort)};
            return std::make_unique<http::Http2Client>(loop, "localhost", kPort,
                                                       resolver, ctx);
        });
    EXPECT_EQ(o.ok, 10) << (o.errors.empty() ? "" : o.errors.front());
    ASSERT_NE(resolver, nullptr);
    EXPECT_EQ(resolver->lookups, 1) << "requests waiting on DNS share one "
                                       "lookup, later ones reuse it";
    EXPECT_EQ(opened, 1u);
}

TEST(Http2ClientTest, DnsFailureFailsWaitingRequests) {
    const uint16_t kPort = 56311;
    std::unique_ptr<FakeResolver> resolver;  // outlives the client
    uint64_t opened = 0;
    uint64_t retries = 0;
    Outcome o = runRequests(
        kPort, 3, {}, &opened, &retries, 0,
        [&](reactor::EventLoop* loop, tls::TlsContextPtr ctx) {
            resolver = std::make_unique<FakeResolver>(loop);  // empty: fails
            return std::make_unique<http::Http2Client>(
                loop, "no-such-host.test", 443, resolver.get(), ctx);
        });
    EXPECT_EQ(o.ok, 0);
    EXPECT_EQ(o.failed, 3);
    ASSERT_FALSE(o.errors.empty());
    EXPECT_NE(o.errors.front().find("DNS resolution failed"), std::string::npos)
        << o.errors.front();
    EXPECT_EQ(resolver->lookups, 1) << "waiting requests share one lookup";
    EXPECT_EQ(opened, 0u) << "no connection without an address";
}

TEST(Http2ClientTest, ResolvesAgainAfterAllAddressesFail) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56312;
    Server server(kPort, cert);

    // First answer points at a dead port; the second at the real server.
    // The client must re-resolve after the dead address fails, instead of
    // using the stale address forever.
    reactor::EventLoop loop;
    auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kClient);
    ctx->setVerifyPeer(false);
    FakeResolver resolver(&loop);
    resolver.answer = {net::InetAddress("127.0.0.1", 56399)};  // nothing there
    http::Http2Client::Options opts;
    opts.connect_timeout_ms = 300;
    opts.max_retries = 0;
    http::Http2Client client(&loop, "localhost", kPort, &resolver, ctx, "",
                             opts);

    std::string first_error;
    int second_status = 0;
    client.get("/a", [&](const http::HttpResponse&, const std::string& err) {
        first_error = err;
        resolver.answer = {net::InetAddress("127.0.0.1", kPort)};
        loop.runAfter(50, [&] {
            client.get("/b",
                       [&](const http::HttpResponse& r, const std::string& e) {
                           second_status = e.empty() ? r.statusCode() : -1;
                           loop.quit();
                       });
        });
    });
    loop.runAfter(10000, [&] { loop.quit(); });
    loop.loop();

    EXPECT_FALSE(first_error.empty());
    EXPECT_EQ(second_status, 200);
    EXPECT_EQ(resolver.lookups, 2) << "a failed connect triggers re-resolve";
}
