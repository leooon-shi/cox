#include "tcp_client.h"

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include <gtest/gtest.h>

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

// Run an echo server on its own EventLoop thread. Caller quits via the returned
// loop pointer.
struct EchoServerThread {
    std::thread thread;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop{nullptr};

    EchoServerThread(uint16_t port)
    {
        thread = std::thread([this, port] {
            reactor::EventLoop lp;
            loop = &lp;
            InetAddress addr("127.0.0.1", port);
            TcpServer server(&lp, addr, "echo-srv");
            server.setMessageCallback(
                [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                    conn->send(buf->retrieveAllAsString());
                });
            server.start();
            started = true;
            lp.loop();
        });
        while (!started.load()) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(20ms);
    }

    void stop() {
        if (loop) loop->quit();
        if (thread.joinable()) thread.join();
    }
};
}   // namespace

TEST(TcpClientTest, ConnectAndEcho)
{
    const uint16_t kPort = 47001;
    EchoServerThread server(kPort);

    std::atomic<bool> cli_started{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> got_reply{false};
    std::string received;
    reactor::EventLoop* cli_loop = nullptr;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        cli_loop = &loop;
        InetAddress server_addr("127.0.0.1", kPort);
        TcpClient client(&loop, server_addr, "echo-cli");
        
        client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if(conn->connected())
            { 
                connected = true;
                conn->send("ping-from-client");
            }
        });

        client.setMessageCallback(
            [&](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
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
    EXPECT_EQ(received, "ping-from-client");

    cli_loop->quit();
    client_thread.join();
    server.stop();
}

TEST(TcpClientTest, RetryUntilServerAvailable)
{
    const uint16_t kPort = 47002;

    // Start the client first, pointing at a port with no server yet.
    std::atomic<bool> cli_started{false};
    std::atomic<bool> connected{false};
    reactor::EventLoop* cli_loop = nullptr;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        cli_loop = &loop;
        InetAddress server_addr("127.0.0.1", kPort);
        TcpClient client(&loop, server_addr, "retry-cli");
        client.enableRetry();
        client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if(conn->connected()) connected = true;
        });
        client.connect();
        cli_started = true;
        loop.loop();
    });
    while (!cli_started.load()) std::this_thread::sleep_for(1ms);

    // Let the client fail at least once and enter backoff.
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(connected.load());

    // Now bring the server up; the client's retry should eventually connect.
    EchoServerThread server(kPort);

    for (int i = 0; i < 400 && !connected.load(); ++i)
    {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(connected.load());

    cli_loop->quit();
    client_thread.join();
    server.stop();
}
