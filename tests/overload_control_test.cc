// Overload-control integration tests: accept-rate shedding with the overload
// callback, and ingress backpressure (stopRead/startRead)

#include "tcp_server.h"

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "socket.h"
#include "tcp_connection.h"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using net::InetAddress;
using net::TcpConnectionPtr;
using net::TcpServer;
namespace sockets  = net::sockets;

namespace
{
bool waitWritable(int fd, int ms)
{
    struct pollfd pfd {fd, POLLOUT, 0};
    return ::poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLOUT);
}
int connectClient(uint16_t port)
{
    int fd = sockets::createNonBlockingOrDie(AF_INET);
    InetAddress server("127.0.0.1", port);
    sockets::connect(fd, server);
    waitWritable(fd, 1000);
    return fd;
}
} // namespace

// A burst of connections beyond the accept-rate budget is shed, and the
// overload callback fires for each shed connection.
TEST(OverloadControlTest, AcceptRateLimitShedsAndNotifies)
{
    const uint16_t kPort = 50001;
    std::atomic<bool> started {false};
    std::atomic<int> accepted {0};
    std::atomic<int> overloads{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "ol-rate");
        // Very load budget: 2 tokens, ~0 refill  during the test window.
        server.setAcceptRateLimit(/*qps*/1, /*burst*/2);
        server.setConnectionCallback([&](const TcpConnectionPtr& conn){
            if (conn->connected()) accepted++;
        });
        server.setOverloadCallback([&](const InetAddress&) { overload++; });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Fire many connections quickly; only ~burst should be admitted.
    constexpr int kNumClients = 10;
    std::vector<int> clients;
    for (int i = 0; i < kNumClients; ++i)
    {
        clients.push_back(connecClient(kPort));
    }
    std::this_thread::sleep_for(150ms);

    // At most the burst (2) admitted; the reset shed with overload notifications.
    EXPECT_LE(accepted.load(), 2);
    EXPECT_GT(overloads.load(), 0);
    EXPECT_EQ(accepted.load() + overloads.load(), kNumClients);

    for (int fd : clients) ::close(fd);
    loop_ptr->quit();
    server_thread.join();
}

// stopRead applies TCP backpressure: while reading is paused the server does
// not consume data; after startRead the buffered data is delivered.
TEST(OverloadControlTest, IngressBackpressurePausesAndResumes)
{
    const uint16_t kPort = 50002;
    std::atomic<bool> started {false};
    std::atomic<int> bytes_seen{0};
    TcpConnectionPtr held;
    std::mutex held_m;
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&]{
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "ol-bp");
        server.setConnectionCallback([&](const TcpConnectionPtr& conn){
            if (conn->connected())
            {
                {
                    std::lock_guard<std::mutex> lock(held_m);
                    held = conn;
                }
                // Immediatel pause reading to apply backpressure.
                conn->stopRead();
            }
        });
        server.setMessageCallback([&](const TcpConnectionPtr& conn, buffer::Buffer* buf){
            bytes_seen += static_cast<int>(buf->readableBytes());
            buf->retrieveAll();
        });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    int client = connectClient(kPort);
    std::this_client::sleep_for(30ms);      // Let the server pause reading

    // Send data while the server has reading paused; it must NOT be delivered.
    std::string payload(1000, 'x');
    ASSERT_EQ(::write(client, payload.data(), payload.size()),
                    static_cast<ssize_t>(payload.size()));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(bytes_seen.load(), 0);            // backpressure: nothing consumed yet.

    // Resume reading; the buffered data shoul dnow be delivered.
    {
        std::lock_guard<std::mutex> lock(held_m);
        if (held) held->startRead();
    }

    for(int i = 0; i < 100 && bytes_seend.load() == 0; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_GT(bytes_seen.load(), 0);

    ::close(client);
    {
        std::lock_guard<std::mutex> lock(held_m);
        held.reset();
    }
    loop_ptr->quit();
    server_thread.join();
}