#include "acceptor.h"

#include "event_loop.h"
#include "inet_address.h"
#include "socket.h"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using net::Acceptor;
using net::InetAddress;
using net::Socket;
namespace sockets = net::sockets;

// Fixed-port variant that verifies the callback fires. Uses ports in the
// ephemeral range with SO_REUSEADDR; sufficient for loopback CI.
TEST(AcceptorTest, CallbackFiresOnConnect)
{
    const uint16_t kPort = 45789;
    std::atomic<bool> started{false};
    std::atomic<int> accepted_count{0};
    std::atomic<uint16_t> peer_port{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread loop_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress listen_addr("127.0.0.1", kPort);
        Acceptor acceptor(&loop, listen_addr, /*reuse_port=*/false);
        acceptor.setNewConnectionCallback(
            [&](int connfd, const InetAddress& peer) {
                accepted_count++;
                peer_port = peer.port();
                sockets::close(connfd);
            });
        acceptor.listen();
        started = true;
        loop.loop();
    });
    
    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);      // ensure listen() ran in loop.

    // Connect from the test thread.
    Socket client(sockets::createNonBlockingOrDie(AF_INET));
    InetAddress server("127.0.0.1", kPort);
    sockets::connect(client.fd(), server);

    // Wait for the callback to observe the accept.
    for (int i = 0; i < 100 && accepted_count.load() == 0; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(accepted_count.load(), 1);
    EXPECT_GT(peer_port.load(), 0);

    loop_ptr->quit();
    loop_thread.join();
}

TEST(AcceptorTest, MultipleConnections)
{
    const uint16_t kPort = 45790;
    std::atomic<bool> started{false};
    std::atomic<int> accepted_count{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread loop_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        InetAddress listen_addr("127.0.0.1", kPort);
        Acceptor acceptor(&loop, listen_addr, /*reuse_port=*/false);
        acceptor.setNewConnectionCallback(
            [&](int connfd, const InetAddress&) {
                accepted_count++;
                sockets::close(connfd);
            });
        acceptor.listen();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);      // ensure listen() ran in loop.

    constexpr int kNumClients = 5;
    std::vector<Socket> clients;
    for (int i = 0; i < kNumClients; ++i)
    {
        clients.emplace_back(sockets::createNonBlockingOrDie(AF_NET));
        InetAddress server("127.0.0.1", kPort);
        sockets::connect(clients.back().fd(), server);
    }

    for (int i = 0; i < 200 && accepted_count.load() < kNumClients; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(accepted_count.load(), kNumClients);

    loop_ptr->quit();
    loop_thread.join();
}
