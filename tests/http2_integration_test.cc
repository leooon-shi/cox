// End-to-end HTTP/2: a real Cox HttpServer (TLS, ALPN h2 + http/1.1) driven
// by curl. Only built when COX_WITH_HTTP2 is ON. OPAQUE: shells out to the
// openssl CLI (cert) and curl (client).

#include "http_request.h"
#include "http_response.h"
#include "http_server.h"

#include "event_loop.h"
#include "inet_address.h"
#include "tcp_connection.h"
#include "tls_context.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct TestCert {
    std::string cert = "/tmp/cox_h2_cert.pem";
    std::string key = "/tmp/cox_h2_key.pem";
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

// Run a shell command and capture stdout
std::string capture(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen(cmd.c_str(), "r");
    if (p == nullptr) {
        return out;
    }
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) {
        out.append(buf, n);
    }
    ::pclose(p);
    return out;
}

bool curHasHttp2() {
    return capture("curl --version 2>/dev/null").find("HTTP2") !=
           std::string::npos;
}

// Runs an HTTPS server on its own loop thread for the lifetime of the oject.
class Server {
public:
    Server(uint16_t port, const TestCert& cert,
           std::vector<std::string> alpn = {"h2", "http/1.1"},
           http::HttpServer::Http2Options h2 = {}, uint32_t max_age = 0) {
        thread_ = std::thread([this, port, &cert, alpn, h2, max_age] {
            reactor::EventLoop loop;
            loop_ = &1oop;
            auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kServer);
            ctx->usecertificateAndKey(cert.cert, cert.key);
            ctx->setAlpnProtocols(apln);
            http::HttpServer server(&loop, net::InetAddress("127.0.0.1", port),
                                    "h2", ctx);
            server.setMaxBodySize(1024);
            server.setHttp2Options(h2);
            if (max_age > 0) {
                server.tcpServer().setMaxConnectionAge(max_age);
            }
            server_ = &server;
            server.setConnectionObserver(
                [this](const net::TcpConnectionPtr& c) {
                    if (c->connected()) {
                        connections_++;
                    } else if (c->closeReason() ==
                               net::TcpConnection::CloseReason::kMaxAge) {
                        max_age_closes_++;
                    }
                });
            server.setHttpHandler(
                [](const http::HttpRequest& req, http::HttpResponse* resp) {
                    if (req.path() == "/slow") {
                        // Holds the stream open across a shutdown.
                        std::this_thread::sleep_for(300ms);
                    }
                    if (req.path() == "/big") {
                        resp->setStatus(200);
                        resp->setBody(std::string(300 * 1024, 'x'));
                        return;
                    }
                    resp->setStatus(200);
                    resp->setContentType("text/plain");
                    resp->setBody(req.version() + " " + req.path() +
                                  " body=" + req.body());
                });
            server.setRequestObserver([this](int status, double) {
                if (status >= 400) {
                    errors_++;
                } else {
                    ok_++;
                }
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
    // Graceful drain (TcpServer::shutdown), run on the server's loop.
    void drain(uint32_t deadline_seconds) {
        loop_->runInLoop([this, deadline_seconds] {
            server_->tcpServer().shutdown(deadline_seconds,
                                          [this] { drained_ = true; });
        });
    }
    // Connections the HttpServer still tracks (read on its loop thread).
    size_t tracked() {
        std::atomic<size_t> n{SIZE_MAX};
        loop_->runInLoop([&] { n = server_->trackedConnections(); });
        while (n.load() == SIZE_MAX) {
            std::this_thread::sleep_for(1ms);
        }
        return n.load();
    }
    std::atomic<int> ok_{0};
    std::atomic<int> errors_{0};
    std::atomic<int> connections_{0};
    std::atomic<int> max_age_closes_{0};
    std::atomic<bool> drained_{false};

private:
    std::thread thread_;
    std::atomic<bool> started_{false};
    reactor::EventLoop* loop_{nullptr};
    http::HttpServer* server_{nullptr};
};

std::string url(uint16_t port, const std::string& path) {
    return "https://127.0.0.1:" + std::to_string(port) + path;
}
}  // namespace

TEST(Http2IntegrationTest, SameHandlerServesH2AndHttp11) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56201;
    Server server(kPort, cert);

    // %{http_version} reports what curl actually negotiated.
    std::string h2 = capture("curl -sk --http2 -w ' [%{http_version}]' " +
                             url(kPort, "/hello"));
    EXPECT_EQ(h2, "HTTP/2 /hello body= [2]");

    std::string h1 = capture("curl -sk --http1.1 -w ' [%{http_version}]' " +
                             url(kPort, "/hello"));
    EXPECT_EQ(h1, "HTTP/1.1 /hello body= [1.1]");
}

TEST(Http2IntegrationTest, MultiplexesManyRequestsOverOneConnection) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56202;
    Server server(kPort, cert);

    // --parallel with one host reuses a single h2 connection; num_connects
    // counts NEW connections per transfer (only the first should be 1).
    std::string cm =
        "curl -sk --http2 --parallel --parallel-max 20 "
        "-w ' %{http_version} %{num_connects}\\n' -o /dev/null";
    for (int i = 0; i < 20; ++i) {
        cmd += " -o /dev/null " + url(kPort, "/i" + std::to_string(i));
    }
    std::string out = capture(cmd);
    std::istringstream lines(out);
    std::string version;
    int connects = 0;
    int new_conns = 0;
    int responses = 0;
    while (lines >> version >> connects) {
        ++responses;
        EXPECT_EQ(version, "2");
        new_conns += connects;
    }
    EXPECT_EQ(responses, 20);
    EXPECT_EQ(new_conns, 1) << "requests should share one h2 connection";
    EXPECT_EQ(server.ok_.load(), 20);
}

TEST(Http2IntegrationTest, LargeResponseAndPostBody) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56203;
    Server server(kPort, cert);

    std::string size = capture(
        "curl -sk --http2 -o /dev/null "
        "-w '%{size_download}' " +
        url(kPort, "/big"));
    EXPECT_EQ(size, std::to_string(300 * 1024));

    std::string post =
        capture("curl -sk --http2 -d 'k=v' " + url(kPort, "/form"));
    EXPECT_EQ(post, "HTTP/2 /form body=k=v");
}

TEST(Http2IntegrationTest, OversizedBodyRejectedWith413) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56204;
    Server server(kPort, cert);  // max body 1024

    std::string code = capture(
        "head -c 5000 /dev/zero | curl -sk --http2 -o /dev/null "
        "-w '%{http_code} %{http_version}' --data-binary @- " +
        url(kPort, "/up"));
    EXPECT_EQ(code, "413 2");
    EXPECT_EQ(server.errors_.load(), 1);
}

TEST(Http2IntegrationTest, ServerWithoutH2InAlpnStaysOnHttp11) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56205;
    Server server(kPort, cert, {"http/1.1"});

    // curl asks for h2 but the server does not offer it: h1 fallback.
    std::string v = capture(
        "curl -sk --http2 -o /dev/null "
        "-w '%{http_version}' " +
        url(kPort, "/"));
    EXPECT_EQ(v, "1.1");
}

// ---- GOAWAY / connection lifecycle, end to end ----

TEST(Http2IntegrationTest, MaxRequestsPerConnectionMakesClientReconnect) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56206;
    http::HttpServer::Http2Options h2;
    h2.max_requests_per_connection = 3;
    h2.goaway_notice_ms = 50;
    Server server(kPort, cert, {"h2", "http/1.1"}, h2);

    // 9 sequential requests in ONE curl process: after every 3rd the server
    // sends GOAWAY, so curl must open a new connection to continue.
    std::string cmd = "curl -sk --http2 -w '%{response_code}\\n' ";
    for (int i = 0; i < 9; ++i) {
        cmd += " -o /dev/null " + url(kPort, "/r" + std::to_string(i));
    }
    std::string out = capture(cmd);
    std::istringstream lines(out);
    int code = 0;
    int ok = 0;
    while (lines >> version) {
        ok += code == 200;
    }
    EXPECT_EQ(ok, 9) << out;
    EXPECT_EQ(server.ok_.load(), 9);
    EXPECT_GE(server.connections_.load(), 3)
        << "a request cap of 3 forces at least 3 connections for 9 requests";
}

TEST(Http2IntegrationTest, GracefulDrainFinishesInFlightRequest) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56207;
    http::HttpServer::Http2Options h2;
    h2.goaway_notice_ms = 50;
    Server server(kPort, cert, {"h2", "http/1.1"}, h2);

    // Start a slow request, then drain the server while it is in flight.
    std::string result;
    std::thread client([&] {
        result = capture("curl -sk --http2 -w ' %{response_code}' " +
                         url(kPort, "/slow"))
    });
    std::this_thread::sleep_for(150ms);  // request is inside the handler
    server.drain(/*deadline_seconds=*/5);
    client.join();

    // The in-flight request completes despite the drain (GOAWAY lets it
    // finish instead of the connection being half-closed under it).
    EXPECT_EQ(result, "HTTP/2 /slow body= 200");
    for (int i = 0; i < 200 && !server.drained_.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(server.drained_.load()) << "connection never closed";
}

TEST(Http2IntegrationTest, MaxConnectionAgeSendsGoAwayNotHardClose) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56208;
    http::HttpServer::Http2Options h2;
    h2.goaway_notice_ms = 50;
    Server server(kPort, cert, {"h2", "http/1.1"}, h2, /*max_age=*/1);

    // ONE curl process, ONE h2 connection: a fast request, then (after
    // --rate spacing pushes it past the 1 s max age) a slow one. Without
    // GOAWAY the age timer would force-close the connection; with it, the
    // in-flight request finishes and curl reconnects for the next.
    std::string out = capture(
        "curl -sk --http2 --rate 2/s -w '%{response_code} %{num_connects}\\n'"
        " -o /dev/null" +
        url(kPort, "/a") + " -o /dev/null " + url(kPort, "/b") +
        " -o /dev/null" + url(kPort, "/slow") + " -o /dev/null " +
        url(kPort, "/c"));
    std::istringstream lines(out);
    int code = 0;
    int connects = 0;
    int ok = 0;
    int new_conns = 0;
    while (lines >> code >> connects) {
        ok += code == 200;
        new_conns += connects;
    }
    EXPECT_EQ(ok, 4) << out;
    EXPECT_GE(new_conns, 2) << "max age should have recycled the connection";
    EXPECT_GE(server.max_age_closes_.load(), 1);
    EXPECT_EQ(server.errors_.load(), 0);
}

TEST(Http2IntegrationTest, ClosedConnectionsAreReleased) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56209;
    Server server(kPort, cert);

    for (const char* proto : {"--http1.1", "--http2"}) {
        for (int i = 0; i < 3; ++i) {
            capture(std::string("curl -sk ") + proto + " -o /dev/null " +
                    url(kPort, "/"));
        }
    }
    // Each curl process closed its connection; the server must have dropped
    // its per-connection state for all six (not just the h2 ones).
    size_t tracked = 99;
    for (int i = 0; i < 100; ++i) {
        tracked = server.tracked();
        if (tracked == 0) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(tracked, 0u);
    EXPECT_EQ(server.connections_.load(), 6);
}

// ---- Async handlers + streamed responses, end to end ----

TEST(Http2IntegrationTest, StreamedResponseArrivesIncrementally) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56210;

    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;
    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kServer);
        ctx->useCertificateAndKey(cert.cert, cert.key);
        ctx->setAlpnProtocols({"h2", "http/1.1"});
        http::HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort),
                                "sse", ctx);
        // Async + streamed: one event every 100 ms from a timer, after the
        // handler has long returned.
        server.setStreamHandler([&loop](const http::ServerStreamPtr& s) {
            s->writeHead(200, {{"content-type", "text/event-stream"}});
            auto n = std::make_shared<int>(0);
            // The tick re-arms itself through a timer; it captures a WEAK
            // reference to itself (a strong one would be a cycle that keeps
            // the closure, and the stream it holds, alive forever).
            auto tick = std::make_shared<std::function<void()>>();
            std::weak_ptr<std::function<void()>> weak_tick = tick;
            *tick = [s, n, weak_tick, &loop] {
                if (s->aborted()) {
                    return;
                }
                s->write("data: " + std::to_string(++*n) + "\n\n");
                if (*n == 5) {
                    s->end();
                    return;
                }
                if (auto t = weak_tick.lock()) {
                    loop.runAfter(100, [t] { (*t)(); });
                }
            };
            (*tick)();
        });
        server.start();
        started = true;
        loop.loop();
    });
    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(30ms);

    // --no-buffer + a timestamp per line: events must arrive spread over
    // ~400 ms, not all at once at the end.
    std::string out =
        capture("curl -sk --http2 --no-buffer " + url(kPort, "/events") +
                " | while IFS= read -r line; do [ -n \"$line\" ] && "
                "echo \"$(date +%s%3N) $line\"; done");
    std::istringstream lines(out);
    std::vector<long long> stamps;
    std::vector<std::string> events;
    long long t = 0;
    std::string rest;
    while (lines >> t && std::getline(lines, rest)) {
        stamps.push_back(t);
        events.push_back(rest);
    }
    ASSERT_EQ(events.size(), 5u) << out;
    EXPECT_EQ(events.front(), " data: 1");
    EXPECT_EQ(events.back(), " data: 5");
    EXPECT_GE(stamps.back() - stamps.front(), 300)
        << "events must stream as they are written, not arrive together";

    loop_ptr->runInLoop([&] { loop_ptr->quit(); });
    server_thread.join();
}

TEST(Http2IntegrationTest, StreamHandlerStillServesHttp11Synchronously) {
    if (!curlHasHttp2()) {
        GTEST_SKIP() << "curl without HTTP/2 support";
    }
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 56211;

    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;
    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kServer);
        ctx->useCertificateAndKey(cert.cert, cert.key);
        ctx->setAlpnProtocols({"h2", "http/1.1"});
        http::HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort),
                                "mix", ctx);
        server.setStreamHandler([](const http::ServerStreamPtr& s) {
            if (s->request().path() == "/sync") {
                s->writeHead(200);
                s->write("a");
                s->write("b");
                s->end();  // finished before returning: fine on HTTP/1.1
            }
            // "/async": dropped unanswered -> 500 on both protocols
        });
        server.start();
        started = true;
        loop.loop();
    });
    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(30ms);

    EXPECT_EQ(capture("curl -sk --http1.1 " + url(kPort, "/sync")), "ab");
    EXPECT_EQ(capture("curl -sk --http2 " + url(kPort, "/sync")), "ab");
    EXPECT_EQ(capture("curl -sk --http1.1 -o /dev/null -w '%{http_code}' " +
                      url(kPort, "/async")),
              "500");
    EXPECT_EQ(capture("curl -sk --http2 -o /dev/null -w '%{http_code}' " +
                      url(kPort, "/async")),
              "500");

    loop_ptr->runInLoop([&] { loop_ptr->quit(); });
    server_thread.join();
}
