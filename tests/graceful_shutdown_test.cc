// Graceful shutdown / drain tests for TcpServer.
#include "tcp_server.h"

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "socket.h"
#include "tcp_connection.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using net::InetAddress;
using net::TcpConnectionPtr;
using net::TcpServer;
namespace sockets = net::sockets;

namespace
{

bool waitReadable(int fd, int timeout_ms)
{
    struct pollfd pfd{fd, POLLIN, 0};
    return ::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN);
}
bool waitWritable(int fd, int timeout_ms)
{
    struct pollfd pfd{fd, POLLOUT, 0};
    return ::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT);
}

int connectClient(uint16_t port)
{
    int fd = sockets::createNonBlockingOrDie(AF_INET);
    InetAddress server("127.0.0.1", port);
    sockets::connect(fd, server);
    waitWritable(fd, 1000);
    return fd;
}
}  // namespace

// Drain completes once connected clients close; the completion callback fires.
TEST(GracefulShutdownTest, DrainCompletesWhenClientsClose)
{
    const uint16_t kPort = 48001;
    std::atomic<bool> started{false};
    std::atomic<bool> drain_done{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "drain");
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                conn->send(buf->retrieveAllAsString());
            });
        server.start();
        started = true;

        // Trigger shutdown from a timer once clients are expected to be up.
        loop.runAfter(200, [&server, &loop, &drain_done]() {
            server.shutdown(10, [&loop, &drain_done]() {
                drain_done = true;
                loop.quit();   // stop the loop once fully drained
            });
        });

        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Connect a client, exchange a message, then close it after shutdown begins.
    int client = connectClient(kPort);
    const char msg[] = "hi";
    ASSERT_TRUE(test_util::sendAll(client, msg, sizeof(msg)));

    waitReadable(client, 1000);
    char buf[16];
    test_util::drainSome(client, buf, sizeof(buf));

    // Wait past the shutdown trigger, then close the client so drain can finish.
    std::this_thread::sleep_for(250ms);
    ::close(client);

    for (int i = 0; i < 200 && !drain_done.load(); ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_TRUE(drain_done.load());

    server_thread.join();
}

// New connections are refused once draining has started.
TEST(GracefulShutdownTest, RejectsNewConnectionsWhileDraining)
{
    const uint16_t kPort = 48002;
    std::atomic<bool> started{false};
    std::atomic<int> accepted{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "drain-reject");
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected()) accepted++;
        });
        server.start();
        started = true;

        // Begin draining almost immediately (no live connections yet).
        loop.runAfter(30, [&server]() { server.shutdown(5); });
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    // Wait until draining has begun.
    std::this_thread::sleep_for(80ms);

    // Attempt to connect after drain started; the server must not accept it.
    int client = connectClient(kPort);
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(accepted.load(), 0);

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}

// A client that refuses to close is force-closed at the drain deadline, and
// drain then completes.
TEST(GracefulShutdownTest, ForceCloseAtDeadline)
{
    const uint16_t kPort = 48003;
    std::atomic<bool> started{false};
    std::atomic<bool> drain_done{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "drain-force");
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                conn->send(buf->retrieveAllAsString());
            });
        server.start();
        started = true;

        // Short 1s force-close deadline so the test is quick.
        loop.runAfter(150, [&server, &loop, &drain_done]() {
            server.shutdown(1, [&loop, &drain_done]() {
                drain_done = true;
                loop.quit();
            });
        });
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Connect and keep the client open (never close), so only the server's
    // force-close deadline can complete the drain.
    int client = connectClient(kPort);
    ASSERT_TRUE(test_util::sendAll(client, "x", 1));
    waitReadable(client, 1000);
    char buf[8];
    test_util::drainSome(client, buf, sizeof(buf));

    // Do NOT close the client. Drain must still finish via the 1s deadline.
    for (int i = 0; i < 400 && !drain_done.load(); ++i)
    {
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_TRUE(drain_done.load());

    ::close(client);
    server_thread.join();
}
