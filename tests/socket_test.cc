#include "socket.h"

#include "inet_address.h"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>

using net::InetAddress;
using net::Socket;
namespace sockets = net::sockets;

namespace
{

// Bind a listening socket to an ephemeral port on loopback and return its
// actual bound address (so tests don't collide on a fixed port).
Socket makeListener(bool ipv6, InetAddress* bound_out)
{
    sa_family_t fam = ipv6 ? AF_INET6 : AF_INET;
    Socket listener(sockets::createNonBlockingOrDie(fam));
    listener.setReuseAddr(true);
    InetAddress any(0, /*loopback_only=*/true, ipv6);
    listener.bindAddress(any);
    listener.listen();
    *bound_out = sockets::getLocalAddr(listener.fd());
    return listener;
}

// Wait until fd is writable (connect completed) or timeout.
bool waitWritable(int fd, int timeout_ms)
{
    struct pollfd pfd{fd, POLLOUT, 0};
    return ::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT);
}

bool waitReadable(int fd, int timeout_ms)
{
    struct pollfd pfd{fd, POLLIN, 0};
    return ::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN);
}
}   // namespace

TEST(SocketTest, CreatedBindListenIpv4)
{
    InetAddress bound;
    Socket listener = makeListener(/*ipv6=*/false, &bound);
    EXPECT_GT(listener.fd(), 0);
    EXPECT_EQ(bound.toIp(), "127.0.0.1");
    EXPECT_GT(bound.port(), 0);
}

TEST(SocketTest, CreatedBindListenIpv6)
{
    InetAddress bound;
    Socket listener = makeListener(/*ipv6=*/true, &bound);
    EXPECT_GT(listener.fd(), 0);
    EXPECT_TRUE(bound.isIpv6());
    EXPECT_GT(bound.port(), 0);
}

TEST(SocketTest, ConnectAcceptRoundTripIpv4)
{
    InetAddress bound;
    Socket listener = makeListener(/*ipv6=*/false, &bound);

    // Client initiates a non-blocking connect.
    Socket client(sockets::createNonBlockingOrDie(AF_INET));
    int rc = sockets::connect(client.fd(), bound);
    // Non-blocking connect typically returns -1 with EINPROGRESS.
    if (rc < 0)
    {
        EXPECT_EQ(errno, EINPROGRESS);
    }

    // Server accepts.
    ASSERT_TRUE(waitReadable(listener.fd(), 1000));
    InetAddress peer;
    int connfd = listener.accept(&peer);
    ASSERT_GT(connfd, 0);
    Socket conn(connfd);

    // Client connect completes.
    ASSERT_TRUE(waitWritable(client.fd(), 1000));
    EXPECT_EQ(sockets::getSocketError(client.fd()), 0);

    // Peer address seen by the server matches the client's local address.
    InetAddress client_local = sockets::getLocalAddr(client.fd());
    EXPECT_EQ(peer.port(), client_local.port());
}

TEST(SocketTest, DataTransfer)
{
    InetAddress bound;
    Socket listener = makeListener(/*ipv6=*/false, &bound);

    Socket client(sockets::createNonBlockingOrDie(AF_INET));
    sockets::connect(client.fd(), bound);

    ASSERT_TRUE(waitReadable(listener.fd(), 1000));
    InetAddress peer;
    int connfd = listener.accept(&peer);
    ASSERT_GT(connfd, 0);
    Socket conn(connfd);
    ASSERT_TRUE(waitWritable(client.fd(), 1000));

    const char msg[] = "ping";
    ssize_t w = ::write(client.fd(), msg, sizeof(msg));
    ASSERT_EQ(w, static_cast<ssize_t>(sizeof(msg)));

    ASSERT_TRUE(waitReadable(conn.fd(), 1000));
    char buf[16] = {};
    ssize_t r = ::read(conn.fd(), buf, sizeof(buf));
    ASSERT_EQ(r, static_cast<ssize_t>(sizeof(msg)));
    EXPECT_STREQ(buf, "ping");
}

TEST(SocketTest, Options)
{
    Socket s(sockets::createNonBlockingOrDie(AF_INET));
    // These should not crash or error on a fresh socket.
    s.setReuseAddr(true);
    s.setReusePort(true);
    s.setTcpNoDelay(true);
    s.setKeepAlive(true);
    SUCCEED();
}

TEST(SocketTest, MoveSemantics)
{
    Socket a(sockets::createNonBlockingOrDie(AF_INET));
    int fd = a.fd();
    Socket b(std::move(a));
    EXPECT_EQ(b.fd(), fd);
    EXPECT_EQ(a.fd(), -1)   // moved-from is emptied
}

TEST(SocketTest, Release)
{
    Socket s(sockets::createNonBlockingOrDie(AF_INET));
    int fd = s.release();
    EXPECT_GT(fd, 0);
    EXPECT_EQ(s.fd(), -1);
    sockets::close(fd);   // caller now owns it
}
