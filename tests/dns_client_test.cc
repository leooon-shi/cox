// DNS-into-client wiring: HttpClient / ConnectionPool / MultiOriginPool taking
// a hostname + Resolver instead of a numeric address. Uses "localhost" so no
// external network is required.

#include "connection_pool.h"
#include "event_loop.h"
#include "http_client.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"
#include "inet_address.h"
#include "multi_origin_pool.h"
#include "resolver.h"
#include "thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace http;

namespace {
// Runs an HTTP server on its own EventLoop thread; the test drives a client on
// a separate loop, mirroring the existing http_client_test style.
class ServerThread {
public:
    explicit ServerThread(uint16_t port) : port_(port) {}

    void start(HttpServer::HttpHandler handler) {
        thread_ = std::thread([this, handler] {
            reactor::EventLoop loop;
            loop_ = &loop;
            HttpServer server(&loop, net::InetAddress(port_), "dns-test");
            server.setHttpHandler(handler);
            server.start();
            ready_ = true;
            loop.loop();
        });
        while (!ready_.load()) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(20ms);  // let the listen socket settle
    }

    void stop() {
        if (loop_) loop_->runInLoop([this] { loop_->quit(); });
        if (thread_.joinable()) thread_.join();
    }

private:
    uint16_t port_;
    std::atomic<bool> ready_{false};
    reactor::EventLoop* loop_{nullptr};
    std::thread thread_;
};
}  // namespace

TEST(DnsClientTest, HttpClientResolvesHostname) {
    const uint16_t kPort = 18131;
    ServerThread server(kPort);
    server.start([](const HttpRequest& req, HttpResponse* resp) {
        resp->setStatus(200);
        resp->setContentType("text/plain");
        resp->setBody("hello " + req.header("host"));
    });

    concurrent::ThreadPool dns_pool("dns");
    dns_pool.start(2);

    std::atomic<bool> done{false};
    int status = 0;
    std::string body, error, host_hdr_seen;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        net::ThreadPoolResolver resolver(&loop, &dns_pool);
        // Hostname, not a numeric address -- resolved by the client itself.
        HttpClient client(&loop, "localhost", kPort, &resolver);
        client.get("/", [&](const HttpResponse& resp, const std::string& err) {
            status = resp.statusCode();
            body = resp.body();
            error = err;
            done = true;
            loop.quit();
        });
        loop.loop();
    });

    for (int i = 0; i < 400 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    client_thread.join();

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(error.empty()) << "error: " << error;
    EXPECT_EQ(status, 200);
    // The Host header defaults to the DNS name, not the resolved IP.
    EXPECT_EQ(body, "hello localhost");

    server.stop();
    dns_pool.stop();
}

TEST(DnsClientTest, HttpClientReportsResolutionFailure) {
    concurrent::ThreadPool dns_pool("dns");
    dns_pool.start(2);

    std::atomic<bool> done{false};
    std::string error;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        net::ThreadPoolResolver resolver(&loop, &dns_pool);
        HttpClient client(&loop, "no.such.host.invalid.", 80, &resolver);
        client.get("/", [&](const HttpResponse&, const std::string& err) {
            error = err;
            done = true;
            loop.quit();
        });
        loop.loop();
    });

    for (int i = 0; i < 600 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    client_thread.join();

    EXPECT_TRUE(done.load());
    EXPECT_NE(error.find("DNS resolution failed"), std::string::npos)
        << "error: " << error;
}

TEST(DnsClientTest, PoolQueuesRequestsDuringResolution) {
    const uint16_t kPort = 18132;
    ServerThread server(kPort);
    server.start([](const HttpRequest&, HttpResponse* resp) {
        resp->setStatus(200);
        resp->setBody("ok");
    });

    concurrent::ThreadPool dns_pool("dns");
    dns_pool.start(2);

    std::atomic<int> completed{0};
    std::atomic<bool> finished{false};
    bool queued_before_resolve = false;
    bool resolved_after = false;
    std::string first_error;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        net::ThreadPoolResolver resolver(&loop, &dns_pool);
        ConnectionPool pool(&loop, "localhost", kPort, &resolver);
        pool.setMaxConnections(2);

        // Three requests issued back-to-back before DNS can have completed.
        HttpRequest req;
        req.setMethod(Method::kGet);
        req.setPath("/");
        for (int i = 0; i < 3; ++i) {
            pool.request(req,
                         [&](const HttpResponse& resp, const std::string& err) {
                             if (!err.empty() && first_error.empty())
                                 first_error = err;
                             if (resp.statusCode() == 200) ++completed;
                             if (completed.load() == 3) {
                                 resolved_after = pool.resolved();
                                 finished = true;
                                 loop.quit();
                             }
                         });
        }
        // Still unresolved right after issuing: all three are parked on DNS.
        queued_before_resolve = !pool.resolved() && pool.queuedRequests() == 3;

        loop.loop();
    });

    for (int i = 0; i < 600 && !finished.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    client_thread.join();

    EXPECT_TRUE(queued_before_resolve);
    EXPECT_TRUE(finished.load()) << "first error: " << first_error;
    EXPECT_EQ(completed.load(), 3);
    EXPECT_TRUE(resolved_after);

    server.stop();
    dns_pool.stop();
}

TEST(DnsClientTest, MultiOriginPoolResolvesHostname) {
    const uint16_t kPort = 18133;
    ServerThread server(kPort);
    server.start([](const HttpRequest& req, HttpResponse* resp) {
        resp->setStatus(200);
        resp->setBody(req.path());
    });

    concurrent::ThreadPool dns_pool("dns");
    dns_pool.start(2);

    std::atomic<bool> done{false};
    int status = 0;
    std::string body, error;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        net::ThreadPoolResolver resolver(&loop, &dns_pool);
        MultiOriginPool pool(&loop);
        pool.setResolver(&resolver);

        HttpRequest req;
        req.setMethod(Method::kGet);
        req.setPath("/multi");
        pool.request("localhost", kPort, req,
                     [&](const HttpResponse& resp, const std::string& err) {
                         status = resp.statusCode();
                         body = resp.body();
                         error = err;
                         done = true;
                         loop.quit();
                     });
        loop.loop();
    });

    for (int i = 0; i < 400 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    client_thread.join();

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(error.empty()) << "error: " << error;
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, "/multi");

    server.stop();
    dns_pool.stop();
}

TEST(DnsClientTest, NumericOriginIsResolvedImmediately) {
    concurrent::ThreadPool dns_pool("dns");
    dns_pool.start(1);

    std::atomic<bool> checked{false};
    bool numeric_resolved = false;

    std::thread t([&] {
        reactor::EventLoop loop;
        // Numeric origin: no DNS path at all, resolved() true from the start.
        ConnectionPool pool(&loop, net::InetAddress("127.0.0.1", 18134));
        numeric_resolved = pool.resolved();

        checked = true;
        loop.quit();
        loop.loop();
    });

    t.join();

    EXPECT_TRUE(checked.load());
    EXPECT_TRUE(numeric_resolved);
    dns_pool.stop();
}
