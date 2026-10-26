// Integration test: offload blocking work from a TcpServer message callback to
// a worker ThreadPool, then post the result back to the connection's loop via
// runInLoop. Verifies the io loop stays responsive while a "slow" handler runs.

#include "thread_pool.h"

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "socket.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using concurrent::ThreadPool;
using net::InetAddress;
using net::TcpConnectionPtr;
using net::TcpServer;
namespace sockets = net::sockets;

namespace
{
// Blocking helpers for the client side of the tests.
bool waitReadable(int fd, int ms)
{
    struct pollfd pfd{fd, POLLIN, 0};
    return ::poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLIN);
}
bool waitWritable(int fd, int ms)
{
    struct pollfd pfd{fd, POLLOUT, 0};
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

// A handler that "blocks" (sleeps) is offloaded to the pool so it does not stall
// the io loop; the reply is marshaled back with runInLoop. Two clients are
// served concurrently even through each handler takes ~100ms.
TEST(ThreadPoolIntegrationTest, OffloadKeepsLoopResponsive)
{
    const uint16_t kPort = 49001;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    ThreadPool pool("io-offload");
    pool.start(4);

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "offload");
        server.setMessageCallback(
            [&](const TcpConnectionPtr& conn, buffer::Buffer* buf){
                std::string req = buf->retrieveAllAsString();
                // Offload a "blocking" computation to the pool
                pool.run([conn, req]() {
                    std::this_thread::sleep_for(100ms);     // simulate blocking work
                    std::string resp = "processed:" + req;
                    // Post the result back to the connection's own loop.
                    conn->getLoop()->runInLoop(
                        [conn, resp]() {
                            conn->send(resp);
                        });
                });
            });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Two clients issue requests at nearly the same time. If the handler ran
    // inline on the loop, the second would be serialized behind the first
    // (~200ms total). Offloaded to a 4-worker pool, both finish in ~100ms.
    int c1 = connectClient(kPort);
    int c2 = connectClient(kPort);

    auto start = std::chrono::steady_clock::now();
    // Check the writes: an unnoticed short/failed write here would make the
    // timing assertions below meaningless rather than falling outright.
    ASSERT_EQ(::write(c1, "a", 1), 1);
    ASSERT_EQ(::write(c2, "b", 1), 1);

    ASSERT_TRUE(waitReadable(c1, 2000));
    ASSERT_TRUE(waitReadable(c2, 2000));

    char b1[64] = {}, b2[64] = {};
    ssize_t n1 = ::read(c1, b1, sizeof(b1));
    ssize_t n2 = ::read(c2, b2, sizeof(b2));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();
    
    ASSERT_GT(n1, 0);
    ASSERT_GT(n2, 0);
    EXPECT_EQ(std::string(b1, n1), "processed:a");
    EXPECT_EQ(std::string(b2, n2), "processed:b");
    // Concurrency check: both handled in parallel, so well under the ~200ms a
    // serialized (inline) handler would take. Allow generous slack for CI.
    EXPECT_LT(elapsed, 180);

    ::close(c1);
    ::close(c2);
    loop_ptr->quit();
    server_thread.join();
    pool.stop();
}
