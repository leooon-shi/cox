// Integration test: installServerMetrics() wires a real TcpServer's lifecycle
// to standard metrics, chaining onto (not clobbering) the user's callbacks.

#include "server_metrics.h"

#include "buffer.h"
#include "event_loop.h"
#include "http_response.h"
#include "http_server.h"
#include "registry.h"
#include "socket.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using net::InetAddress;
using net::TcpConnectionPtr;
using net::TcpServer;
namespace sockets = net::sockets;

namespace {
bool waitWritable(int fd, int timeout_ms) {
    pollfd pfd{fd, POLLOUT, 0};
    return ::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT);
}
int connectClient(uint16_t port) {
    int fd = sockets::createNonBlockingOrDie(AF_INET);
    InetAddress server("127.0.0.1", port);
    sockets::connect(fd, server);
    waitWritable(fd, 1000);
    return fd;
}

// Find a metric's scalar value in a snapshot by name.
double valueOf(const metrics::Snapshot& snap, const std::string& name) {
    for (const auto& p : snap) {
        if (p.name == name) {
            return p.value;
        }
    }
    return -1;  // sentinel: not present
}
// Find a histogram's total count in a snaphot by name.
uint64_t histCountOf(const metrics::Snapshot& snap, const std::string& name) {
    for (const auto& p : snap) {
        if (p.name == name) {
            return p.hist_count;
        }
    }
    return 0;
}

// Find a labeled counter's value in a snapshot (name + single label match).
double labeledValue(const metrics::Snapshot& snap, const std::string& name,
                    const std::string& lkey, const std::string& lval) {
    for (const auto& p : snap) {
        if (p.name != name || p.labels.size() != 1) {
            continue;
        }
        if (p.labels[0].first == lkey && p.labels[0].second == lval) {
            return p.value;
        }
    }
    return -1;
}
}  // namespace

TEST(MetricsServerTest, CountsAcceptedLiveAndClosedAndChainsUserCallback) {
    const uint16_t kPort = 46210;
    metrics::Registry reg;
    std::atomic<bool> started{false};
    std::atomic<int> user_connect_events{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        TcpServer server(&loop, InetAddress("127.0.0.1", kPort), "m");

        // A user connection callback that metrics must CHAIN, not replace.
        server.setConnectionCallback([&](const TcpConnectionPtr& c) {
            if (c->connected()) {
                user_connect_events++;
            }
        });
        server.setMessageCallback(
            [](const TcpConnectionPtr& c, buffer::Buffer* b) {
                c->send(b->retrieveAllAsString());
            });

        // Install AFTER the user's callbacks, BEFORE start().
        metrics::installHttpMetrics(reg, server);

        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);

    // Open two connections.
    int c1 = connectClient(kPort);
    int c2 = connectClient(kPort);
    std::this_thread::sleep_for(50ms);

    {
        auto snap = reg.scrape();
        EXPECT_EQ(valueOf(snap, "cox_connections_accepted_total"), 2.0);
        EXPECT_EQ(valueOf(snap, "cox_connections_live"), 2.0);
        EXPECT_EQ(valueOf(snap, "cox_connections_closed_total"), 0.0);
    }

    // Close one; live should drop, closed shuld rise.
    ::close(c1);
    std::this_thread::sleep_for(50ms);
    {
        auto snap = reg.scrape();
        EXPECT_EQ(valueOf(snap, "cox_connections_live"), 1.0);
        EXPECT_EQ(valueOf(snap, "cox_connections_closed_total"), 1.0);
    }

    // The user's callback still ran for both connects (chaining works).
    EXPECT_EQ(user_connect_events.load(), 2);

    ::close(c2);
    std::this_thread::sleep_for(20ms);
    loop_ptr->quit();
    server_thread.join();
}

TEST(MetricsServerTest, RejectedCounterOnConnectionCap) {
    const uint16_t kPort = 46211;
    metrics::Registry reg;
    std::atomic<bool> started{false};
    std::atomic<int> user_overloads{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        TcpServer server(&loop, InetAddress("127.0.0.1", kPort), "m");
        server.setMaxConnections(1);  // cap at one
        server.setOverloadCallback(
            [&](const InetAddress&) { user_overloads++; });  // must chain
        metrics::installServerMetrics(reg, server);
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);

    // First connection admitted; subsequent ones exceed the cap -> shed.
    int c1 = connectClient(kPort);
    int c2 = connectClient(kPort);
    int c3 = connectClient(kPort);
    std::this_thread::sleep_for(80ms);

    auto snap = reg.scrape();
    EXPECT_GE(valueOf(snap, "cox_connections_rejected_total"), 1.0);
    EXPECT_GE(user_overloads.load(), 1);  // user overload cb chained

    ::close(c1);
    ::close(c2);
    ::close(c3);
    std::this_thread::sleep_for(20ms);
    loop_ptr->quit();
    server_thread.join();
}

TEST(MetricsServerTest, HtppRequestMetricsByStatusClassAndDuration) {
    const uint16_t kPort = 46212;
    metrics::Registry reg;
    std::atomic<bool> started{false};
    std::atomic<int> user_obs_calls{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        http::HttpServer server(&loop, InetAddress("127.0.0.1", kPort), "h");
        server.setHttpHandler(
            [](const http::HttpRequest& req, http::HttpResponse* resp) {
                if (req.path() == "/ok") {
                    resp->setStatus(200);
                    resp->setBody("ok");
                } else {
                    resp->setStatus(404);
                    resp->setBody("nope");
                }
            });
        // A user observer metrics must CHAIN, not replace
        server.setRequestObserver([&](int, double) { user_obs_calls++; });
        metrics::installHttpMetrics(reg, server);
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(30ms);

    // One 200 and one 404 (each on its own connection: Connection: close).
    for (const char* path : {"/ok", "/missing"}) {
        int fd = connectClient(kPort);
        std::string req = std::string("GET ") + path +
                          "HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        ASSERT_TRUE(test_util::sendAll(fd, req));
        char buf[256];
        (void)::read(fd, buf, sizeof(buf));  // drain response
        ::close(fd);
    }
    std::this_thread::sleep_for(50ms);

    auto snap = reg.scrape();
    EXPECT_EQ(
        labeledValue(snap, "cox_http_requests_total", "status_class", "2xx"),
        1.0);
    EXPECT_EQ(
        labeledValue(snap, "cox_http_requests_total", "status_class", "4xx"),
        1.0);
    EXPECT_EQ(histCountOf(snap, "cox_http_request_duration_seconds"), 2u);
    EXPECT_EQ(user_obs_calls.load(), 2);  // user observer chained

    loop_ptr->quit();
    server_thread.join();
}

TEST(MetricsServerTest, ByteCountersSummedOnClose) {
    const uint16_t kPort = 46313;
    metrics::Registry reg;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        TcpServer server(&loop, InetAddress("127.0.0.1", kPort), "m");
        server.setMessageCallback(  // echo
            [](const TcpConnectionPtr& c, buffer::Buffer* b) {
                c->send(b->retrieveAllAsString());
            });
        metrics::installServerMetrics(reg, server);
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);

    const std::string msg = "0123456789";  // 10 bytes
    int fd = connectClient(kPort);
    ASSERT_TRUE(test_util::sendAll(fd, msg));
    char buf[64];
    (void)::read(fd, buf, sizeof(buf));  // read the echo back
    std::this_thread::sleep_for(30ms);
    ::close(fd);  // triggers the close-time fold
    std::this_thread::sleep_for(50ms);

    auto snap = reg.scrape();
    EXPECT_EQ(valueOf(snap, "cox_connection_bytes_in_total"), 10.0);
    EXPECT_EQ(valueOf(snap, "cox_connection_bytes_out_total"), 10.0);

    // Quit on the loop thread so connection teardown (handleClose) completes
    // before the loop/server destruct, matching real shutdown ordering.
    loop_ptr->runInLoop([&] { loop_ptr->quit(); });
    server_thread.join();
}

TEST(MetricsServerTest, LoopMetricsSampleLiveCounters) {
    metrics::Registry reg;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread loop_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        metrics::installLoopMetrics(reg, loop, {{"loop", "test"}});
        // Keep the loop busy so iterations advance: a repeating timer.
        loop.runEvery(2, [] {});
        started = true;
        loop.loop();
    });

    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(100ms);  // let several iterations happen

    auto snap1 = reg.scrape();
    double iters1 = valueOf(snap1, "cox_loop_iterations_total");
    EXPECT_GT(iters1, 0.0);  // loop has iterated

    std::this_thread::sleep_for(100ms);
    auto snap2 = reg.scrape();
    double iters2 = valueOf(snap2, "cox_loop_iterations_total");
    EXPECT_GT(iters2, iters1);  // sampled live -> advances between scrapes

    // poll-wait accumulates (seconds; small but > 0 after blocking in poll).
    EXPECT_GE(valueOf(snap2, "cox_loop_wait_seconds_total"), 0.0);

    loop_ptr->runInLoop([&] { loop_ptr->quit(); });
    loop_thread.join();
}
