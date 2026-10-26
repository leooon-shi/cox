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
#include <set>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using net::InetAddress;
using net::Socket;
using net::TcpConnectionPtr;
using net::TcpServer;
namespace sockets = net::sockets;

namespace
{
// Blocking helpers for the client side of the tests.
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

// Connect a blocking client socket to a loopback port.
int connectClient(uint16_t port)
{
    int fd = sockets::createNonBlockingOrDie(AF_INET);
    InetAddress server("127.0.0.1", port);
    sockets::connect(fd, server);
    waitWritable(fd, 1000);
    return fd;
}

}   // namespace

TEST(TcpServerTest, EchoRoundTrip)
{
    const uint16_t kPort = 46001;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "echo");
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                // Echo everything back.
                conn->send(buf->retrieveAllAsString());
            });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    int client = connectClient(kPort);
    const std::string msg = "hello echo server";
    ASSERT_EQ(::write(client, msg.data(), msg.size()),
              static_cast<ssize_t>(msg.size()));

    ASSERT_TRUE(waitReadable(client, 1000));
    char buf[128] = {};
    ssize_t n = ::read(client, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, n), msg);

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}

TEST(TcpServerTest, ConnectionCallbackFires)
{
    const uint16_t kPort = 46002;
    std::atomic<bool> started{false};
    std::atomic<int> connect_events{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "cb");
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            // Fires on both connect and disconnect; count connects only.
            if (conn->connected())
            {
                connect_events++;
            }
        });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    int client = connectClient(kPort);
    for (int i = 0; i < 100 && connect_events.load() == 0; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(connect_events.load(), 1);

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}

TEST(TcpServerTest, MultipleClientsEcho)
{
    const uint16_t kPort = 46003;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "echo-multi");
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                conn->send(buf->retrieveAllAsString());
            });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    constexpr int kNumClients = 4;
    std::vector<int> clients;
    for (int i = 0; i < kNumClients; ++i)
    {
        clients.push_back(connectClient(kPort));
    }

    for (int i = 0; i < kNumClients; ++i)
    {
        std::string msg = "client-" + std::to_string(i);
        ASSERT_EQ(::write(clients[i], msg.data(), msg.size()),
                  static_cast<ssize_t>(msg.size()));
    }

    for (int i = 0; i < kNumClients; ++i)
    {
        ASSERT_TRUE(waitReadable(clients[i], 1000));
        char buf[64] = {};
        ssize_t n = ::read(clients[i], buf, sizeof(buf));
        ASSERT_GT(n, 0);
        EXPECT_EQ(std::string(buf, n), "client-" + std::to_string(i));
        ::close(clients[i]);
    }

    loop_ptr->quit();
    server_thread.join();
}

TEST(TcpServerTest, IdleTimeoutClosesSilentClient)
{
    const uint16_t kPort = 46005;
    std::atomic<bool> started{false};
    std::atomic<int> disconnects{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "idle");
        server.setIdleTimeout(1);  // 1 second idle timeout
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (!conn->connected())
            {
                disconnects++;
            }
        });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    int client = connectClient(kPort);

    // Within a few seconds the server's idle timeout should force-close the
    // connection. The bound is generous on purpose: the idle timeout is 1s, but
    // when the whole suite runs in parallel this machine is loaded and timer
    // dispatch can slip. A tight bound made this test flaky under `ctest -j8`
    // while passing in isolation.
    for (int i = 0; i < 1000 && disconnects.load() == 0; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(disconnects.load(), 1);

    // The client should observe the close (read returns 0).
    char buf[8];
    ssize_t n = ::read(client, buf, sizeof(buf));
    EXPECT_EQ(n, 0);

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}

TEST(TcpServerTest, IdleTimeoutResetOnActivity)
{
    const uint16_t kPort = 46006;
    std::atomic<bool> started{false};
    std::atomic<int> disconnects{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "idle-reset");
        server.setIdleTimeout(1);
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                conn->send(buf->retrieveAllAsString());
            });
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (!conn->connected())
            {
                disconnects++;
            }
        });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Connect but send nothing (slowloris-style silent hold).
    int client = connectClient(kPort);
    // Send a byte every 400ms for ~1.6s: activity keeps resetting the 1s timer,
    // so the connection must survive the whole period.
    for (int i = 0; i < 4; ++i)
    {
        const char c = 'x';
        ASSERT_EQ(::write(client, &c, 1), 1);
        ASSERT_TRUE(waitReadable(client, 1000));
        char buf[8];
        // Drain the echo; content is irrelevant here, the point is the activity
        // resetting the idle timer.
        ASSERT_GT(::read(client, buf, sizeof(buf)), 0);
        std::this_thread::sleep_for(400ms);
    }
    // Still connected because activity kept resetting the idle timer.
    EXPECT_EQ(disconnects.load(), 0);

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}

TEST(TcpServerTest, MaxConnectionsCap)
{
    const uint16_t kPort = 46008;
    const size_t kMax = 3;
    std::atomic<bool> started{false};
    std::atomic<int> live_conns{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "cap");
        server.setMaxConnections(kMax);

        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected())
            {
                live_conns++;
            }
            else
            {
                live_conns--;
            }
        });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Open more clients than the cap allows.
    constexpr int kNumClients = 6;
    std::vector<int> clients;
    for (int i = 0; i < kNumClients; ++i)
    {
        clients.push_back(connectClient(kPort));
    }
    std::this_thread::sleep_for(100ms);

    // At most kMax connections should be live on the server.
    EXPECT_LE(live_conns.load(), static_cast<int>(kMax));

    for (int fd : clients)
    {
        ::close(fd);
    }
    loop_ptr->quit();
    server_thread.join();
}

TEST(TcpServerTest, WriteToClosedClientDoesNotCrash)
{
    // Regression for SIGPIPE handling: if the server writes to a connection
    // whose peer has gone away, the process must not be killed by SIGPIPE.
    const uint16_t kPort = 46007;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "sigpipe");
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                buf->retrieveAll();
                // Reply with a large payload; the client will have gone away.
                conn->send(std::string(1024 * 1024, 'y'));
            });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    int client = connectClient(kPort);

    const char req = 'q';
    ASSERT_EQ(::write(client, &req, 1), 1);
    // Immediately abandon the client so the server's write hits a closed peer.
    ::close(client);

    // Give the server time to attempt the write(s). If SIGPIPE were not
    // ignored, the process would have died and this test would not complete.
    std::this_thread::sleep_for(200ms);
    SUCCEED();

    loop_ptr->quit();
    server_thread.join();
}

TEST(TcpServerTest, ServerDetectsClientClose)
{
    const uint16_t kPort = 46004;
    std::atomic<bool> started{false};
    std::atomic<int> disconnects{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "close");
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (!conn->connected())
            {
                disconnects++;
            }
        });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    int client = connectClient(kPort);
    std::this_thread::sleep_for(30ms);
    ::close(client);  // client closes -> server should see EOF

    for (int i = 0; i < 100 && disconnects.load() == 0; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(disconnects.load(), 1);
    
    loop_ptr->quit();
    server_thread.join();
}

TEST(TcpServerTest, MultiThreadedEcho)
{
    const uint16_t kPort = 46009;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "mt-echo");
        server.setThreadNum(3);  // distribute connections across 3 io_loops
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                conn->send(buf->retrieveAllAsString());
            });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    // Many concurrent clients; each should get its own message echoed back,
    // regardless of which io loop handled it.
    constexpr int kNumClients = 12;
    std::vector<int> clients;
    for (int i = 0; i < kNumClients; ++i)
    {
        clients.push_back(connectClient(kPort));
    }

    for(int i = 0; i < kNumClients; ++i)
    {
        std::string msg = "mt-" + std::to_string(i);
        ASSERT_EQ(::write(clients[i], msg.data(), msg.size()),
                  static_cast<ssize_t>(msg.size()));
    }

    for (int i = 0; i < kNumClients; ++i)
    {
        ASSERT_TRUE(waitReadable(clients[i], 1000));
        char buf[64] = {};
        ssize_t n = ::read(clients[i], buf, sizeof(buf));
        ASSERT_GT(n, 0);
        EXPECT_EQ(std::string(buf, n), "mt-" + std::to_string(i));
        ::close(clients[i]);
    }

    loop_ptr->quit();
    server_thread.join();
}

TEST(TcpServerTest, MultiThreadedConnectionsUseDistinctLoops)
{
    const uint16_t kPort = 46010;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::mutex m;
    std::set<std::thread::id> handler_threads;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "mt-loops");
        server.setThreadNum(3);
        server.setMessageCallback(
            [&](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                // Record which thread handled this connection's I/O.
                {
                    std::lock_guard<std::mutex> lock(m);
                    handler_threads.insert(std::this_thread::get_id());
                }
                conn->send(buf->retrieveAllAsString());
            });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    // Enough clients to very likely hit all 3 io loops via round-robin.
    constexpr int kNumClients = 9;
    std::vector<int> clients;
    for (int i = 0; i < kNumberClients; ++i)
    {
        clients.push_back(connectClient(kPort));
    }

    for (int fd : clients)
    {
        const char c = 'z';
        EXPECT_EQ(::write(fd, &c, 1), 1);
    }

    for (int fd : clients)
    {
        waitReadable(fd, 1000);
        char buf[8];
        // Best-effort drain during teardown: a connection may already be gone,
        // so this is intentionally not asserted, just not ignored.
        ssize_t n = ::read(fd, buf, sizeof(buf));
        (void)n;
        ::close(fd);
    }

    // Round-robin over 3 loops with 9 connections should exercise >1 thread.
    {
        std::lock_guard<std::mutex> lock(m);
        EXPECT_GE(handler_threads.size(), 2u);
    }

    loop_ptr->quit();
    server_thread.join();
}
