// End-to-end tests for Unix-domain (AF_UNIX) sockets, verifying that the same
// TcpServer / TcpClient machinery works over a filesystem socket path -- the
// efficient IPC path for processes on the same host or in the same container.

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_client.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using net::InetAddress;
using net::TcpClient;
using net::TcpConnectionPtr;
using net::TcpServer;

namespace
{
std::string uniquePath(const char* tag)
{
    return std::string("/tmp/cox_uds_") + tag + "_" +
            std::to_string(::getpid()) + ".sock";
}
} // namespace

TEST(UnixSocketTest, EchoOverUnixDomain)
{
    const std::string path = uniquePath("echo");
    ::unlink(path.c_str());

    std::atomic<bool> srv_started{false};
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;
        InetAddress addr = InetAddress::unixDomain(path);
        TcpServer server(&loop, addr, "uds-echo");
        server.setMessageCallback([](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
            conn->send(buf->retrieveAllAsString());
        });
        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    std::atomic<bool> cli_started {false};
    std::atomic<bool> connected {false};
    std::atomic<bool> got_reply{false};
    std::string received;
    reactor::EventLoop* cli_loop = nullptr;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        cli_loop = &loop;
        InetAddress addr = InetAddress::unixDomain(path);
        TcpClient client(&loop, addr, "uds-cli");
        client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected())
            {
                connected = true;
                conn->send("hello over unix socket");
            }
        });
        client.setMessageCallback([&](const TcpConnectionPtr& conn, buffer::Buffer* buf){
            received = buf->retrieveAllAsString();
            got_reply = true;
        });

        client.connect();
        cli_started = true;
        loop.loop();
    });

    while (!cli_started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 200 && !got_reply.load(); ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_TRUE(connected.load());
    EXPECT_TRUE(got_reply.load());
    EXPECT_EQ(received, "hello over unix socket");

    cli_loop->quit();
    client_thread.join();
    srv_loop->quit();
    server_thread.join();
    ::unlink(path.c_str());
}

TEST(UnixSocketTest, RetryUntilServerAvailable)
{
    const std::string path = uniquePath("retry");
    ::unlink(path.c_str());         // Ensure the path does not exist yet.

    // Start the client first, pointing at a socket path with no server. The
    // initial connect fails with ENOENT; with rery enabled the Connector must
    // back off and try again rather than give up.
    std::atomic<bool> cli_started{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> got_reply{false};
    reactor::EventLoop* cli_loop = nullptr;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        cli_loop = &loop;
        InetAddress addr = InetAddress::unixDomain(path);
        TcpClient client(&loop, addr, "uds-retry-cli");
        client.enableRetry();
        client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected())
            {
                connected = true;
                conn->send("late hello");
            }
        });
        client.setMessageCallback(
            [&](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                buf->retrieveAll();
                got_reply = true;
            });
        client.connect();
        cli_started = true;
        loop.loop();
    });

    while( !cli_started.load()) std::this_thread::sleep_for(1ms);

    // Let the client fail at least once and enter backoff.
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(connected.load());

    // Now bring the Unix-domain echo server up on the same path.
    std::atomic<bool> srv_started {false};
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;
        InetAddress addr = InetAddress::unixDomain(path);
        TcpServer server(&loop, addr, "uds-retry-srv");
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                conn->send(buf->retrieveAllAsstring());
        });
        server.start();
        srv_started = true;
        loop.loop();
    });
    while (!srv_started.load()) std::this_thread::sleep_for(1ms);

    // The client's retry should eventually connect and complete the round-trip.
    for (int i = 0; i < 400 && !got_reply.load(); ++i)
    {
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_TRUE(connected.load());
    EXPECT_TRUE(got_reply.load());

    cli_loop->quit();
    client_thread.join();
    srv_loop->quit();
    server_thread.join();
    ::unlink(path.c_str());
}

TEST(UnixSocketTest, MultipleClientOverUnixDomain)
{
    const std::string path = uniquePath("multi");
    ::unlink(path.c_str());

    std::atomic<bool> srv_started{false};
    std::atomic<int> accepted {0};
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;
        InetAddress addr = InetAddress::unixDomain(path);
        TcpServer server(&loop, addr, "uds-multi");
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected()) accepted++;
        });
        server.setMessageCallback([](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
            conn->send(buf->retrieveAllAsString());
        });

        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    constexpr int kNumClients = 3;
    std::vector<std::thread> client_threads;
    std::vector<reactor::EventLoop*> client_loops(kNumClients, nullptr);
    std::atomic<int> replies {0};

    for (int i = 0; i < kNumClients; ++i)
    {
        client_threads.emplace_back([&, i] {
            reactor::EventLoop loop;
            client_loops[i] = &loop;
            InetAddress addr = InetAddress::unixDomain(path);
            TcpClient client(&loop, addr, "uds-cli-" + std::to_string(i));
            client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
                if (conn->connected()) conn->send("hi");
            });
            client.setMessageCallback([&](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                buf->retrieveAll();
                replies++;
            });
            client.connect();
            loop.loop();
        });
    }

    for( int i = 0; i < 200 && replies.load() < kNumClients; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(accepted.load(), kNumClients);
    EXPECT_EQ(replies.load(), kNumClients);

    for (int i = 0; i < kNumClients; ++i)
    {
        while (!client_loops[i]) std::this_thread::sleep_for(1ms);
        client_loops[i]->quit();
        client_threads[i].join();
    }
    srv_loop->quit();
    server_thread.join();
    ::unlink(path.c_str());
}