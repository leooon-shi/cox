// Tests for the HTTP connection pool: concurrent dispatch, connection cap +
// queueing, and reuse of idle connections.

#include "connection_pool.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"

#include "event_loop.h"
#include "inet_address.h"
#include "tcp_connection.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace http;

namespace {
HttpRequest getReq(const std::string& path) {
    HttpRequest r;
    r.setMethod(Method::kGet);
    r.setPath(path);
    return r;
}
}  // namespace

// Fire several concurrent requests through a pool; all should complete, and the
// pool should open no more than maxConnections.
TEST(ConnectionPoolTest, ConcurrentRequestsRespectCap) {
    const uint16_t kPort = 55701;
    std::atomic<bool> srv_started{false};
    std::atomic<int> server_conns{0};
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;

        HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort), "srv");
        server.setConnectionObserver([&](const net::TcpConnectionPtr& c) {
            if (c->connected()) {
                server_conns++;
            }
        });
        server.setHttpHandler([](const HttpRequest& req, HttpResponse* resp) {
            resp->setStatus(200);
            resp->setBody("r:" + req.path());
        });
        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(30ms);

    constexpr int kNumRequests = 12;
    const size_t kMaxConns = 3;
    std::atomic<int> completed{0};
    std::atomic<bool> cli_started{false};

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        auto pool = std::make_shared<ConnectionPool>(
            &loop, net::InetAddress("127.0.0.1", kPort), "localhost");
        pool->setMaxConnections(kMaxConns);

        for (int i = 0; i < kNumRequests; ++i) {
            pool->request(
                getReq("/r" + std::to_string(i)),
                [&](const HttpResponse& resp, const std::string& err) {
                    if (err.empty() && resp.statusCode() == 200) {
                        completed++;
                    }
                    if (completed.load() == kNumRequests) {
                        loop.quit();
                    }
                });
        }
        cli_started = true;
        loop.loop();
        // Keep pool alive until loop ends.
        (void)pool;
    });

    while (!cli_started.load()) {
        std::this_thread::sleep_for(1ms);
    }

    for (int i = 0; i < 400 && completed.load() < kNumRequests; ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(completed.load(), kNumRequests);
    // The pool must never exceed the configured connection cap.
    EXPECT_LE(server_conns.load(), static_cast<int>(kMaxConns));
    EXPECT_GT(server_conns.load(), 0);

    client_thread.join();
    srv_loop->quit();
    server_thread.join();
}

// Sequential requests should reuse a single pooled connection.
TEST(ConnectionPoolTest, SequentialReuseSingleConnection) {
    const uint16_t kPort = 55702;
    std::atomic<bool> srv_started{false};
    std::atomic<int> server_conns{0};
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;

        HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort), "srv");
        server.setConnectionObserver([&](const net::TcpConnectionPtr& c) {
            if (c->connected()) {
                server_conns++;
            }
        });
        server.setHttpHandler([](const HttpRequest& req, HttpResponse* resp) {
            resp->setStatus(200);
            resp->setBody("r:" + req.path());
        });
        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(30ms);

    std::atomic<int> completed{0};
    std::atomic<bool> cli_started{false};

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        auto pool = std::make_shared<ConnectionPool>(
            &loop, net::InetAddress("127.0.0.1", kPort), "localhost");
        pool->setMaxConnections(4);

        // Chain 3 sequential requests (each issued from the prior's callback).
        std::function<void(int)> fire = [&](int n) {
            if (n == 0) {
                loop.quit();
                return;
            }
            pool->request(getReq("/s"),
                          [&, n](const HttpResponse&, const std::string& err) {
                              if (err.empty()) {
                                  completed++;
                              }
                              fire(n - 1);
                          });
        };
        fire(3);
        cli_started = true;
        loop.loop();
        (void)pool;
    });

    while (!cli_started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    for (int i = 0; i < 400 && completed.load() < 3; ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(completed.load(), 3);
    // Sequential reuse: a single connection served all three requests.
    EXPECT_EQ(server_conns.load(), 1);

    client_thread.join();
    srv_loop->quit();
    server_thread.join();
}
