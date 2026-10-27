// Deferred-hardening tests: connection max-age, max input buffer, write
// timeout.
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

using namespace std::chrono_literals;
using net::InetAddress;
using net::TcpConnectionPtr;
using net::TcpServer;
namespace sockets = net::sockets;

namespace {
bool waitReadable(int fd, int ms) {
    pollfd pfd{fd, POLLIN, 0};
    return ::poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLIN);
}

bool waitWritable(int fd, int ms) {
    pollfd pfd{fd, POLLOUT, 0};
    return ::poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLOUT);
}

int connectClient(uint16_t port) {
    int fd = sockets::createNonBlockingOrDie(AF_INET);
    InetAddress server("127.0.0.1", port);
    sockets::connect(fd, server);
    waitWritable(fd, 1000);
    return fd;
}
}  // namespace

// A connection is force-closed once it reaches the configured max age, even if
// it is actively exchanging data.
TEST(HardeningTest, MaxConnectionAgeRecyclesConnection) {
    const uint16_t kPort = 51001;
    std::atomic<bool> started{false};
    std::atomic<int> disconnects{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "maxage");
        server.setMaxConnectionAge(1);  // 1 second lifetime
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (!conn->connected()) disconnects++;
        });
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

    int client = connectClient(kPort);
    // Keep the connection active with periodic traffic; max-age must still
    // fire. The window is generous (max-age is 1s): under `ctest -j8` this
    // machine is loaded and timer dispatch slips, which made a 1.5s bound flaky
    // while it passed in isolation.
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 8000ms &&
           disconnects.load() == 0) {
        ASSERT_TRUE(test_util::sendAll(client, "x", 1));
        char b[8];
        if (waitReadable(client, 100))
            test_util::drainSome(client, b, sizeof(b));
        std::this_thread::sleep_for(100ms);
    }

    EXPECT_EQ(disconnects.load(), 1);  // recycled despite activity

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}

// If the peer floods data that the application does not consume, the input
// buffer cap force-closes the connection.
TEST(HardeningTest, MaxInputBufferClosesFlood) {
    const uint16_t kPort = 51002;
    std::atomic<bool> started{false};
    std::atomic<int> disconnects{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "maxinput");
        server.setMaxInputBufferSize(4096);  // small cap.
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (!conn->connected()) disconnects++;
        });
        // Message callback that does NOT consume the buffer -> it grows.
        server.setMessageCallback([](const TcpConnectionPtr&, buffer::Buffer*) {
            // intentionally leave data in the input buffer
        });

        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    int client = connectClient(kPort);
    // Send more than the cap; the server should force-close.
    std::string payload(16384, 'x');
    ASSERT_TRUE(test_util::sendAll(client, payload));

    for (int i = 0; i < 200 && disconnects.load() == 0; ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(disconnects.load(), 1);
    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}

// A slow consumer (client that never reads) whose output cannot drain is
// force-closed at the write-timeout deadline.
TEST(HardeningTest, WriteTimeoutClosesSlowConsumer) {
    const uint16_t kPort = 51003;
    std::atomic<bool> started{false};
    std::atomic<int> disconnects{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "writeto");
        server.setWriteTimeout(1);  // 1 second to drain output
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                // Push a large payload to a client that will not read it,
                // forcing the output buffer to back up.
                conn->send(std::string(8 * 1024 * 1024, 'y'));  // 8 MB
            } else {
                disconnects++;
            }
        });

        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Connect but never read: the server's socket send buffer fills, output
    // backs up, and the write timeout must fire.
    int client = connectClient(kPort);

    for (int i = 0; i < 400 && disconnects.load() == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(disconnects.load(), 1);

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}
