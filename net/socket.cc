#include "socket.h"

#include "inet_address.h"
#include "logger.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace net
{
namespace {
logging::Logger& log() {
    static logging::Logger logger;
    return logger;
}
} // namespace

namespace sockets
{
int createNonBlockingOrDie(sa_family_t family) {
    // Unix-domain sockets use protocol 0; IP stream sockets use TCP.
    int protocol = (family == AF_UNIX) ? 0 : IPPROTO_TCP;
    int sockfd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          protocol);

    if (sockfd < 0 ) {
        log().fatal("sockets::createNonBlockingOrDie failed: " +
                    std::string(std::strerror(errno)));
        std::abort();
    }
    return sockfd;
}

void bindOrDie(int sockfd, const InetAddress& addr) {
    // For pathname Unix sockets, remove any stale socket file left by a prior
    // run; otherwise bind fails with EADDRINUSE.
    if (addr.isUnix() && !addr.unixPath().empty()) {
        ::unlink(addr.unixPath().c_str());
    }

    if (::bind(sockfd, addr.getSockAddr(), addr.getSockLen()) < 0)
    {
        log().fatal("sockets::bindOrDie " + addr.toIpPort() + " failed: " +
                    std::string(std::strerror(errno)));
        std::abort();
    }
}

void listenOrDie(int sockfd)
{
    if (::listen(sockfd, SOMAXCONN) < 0 ) {
        log().fatal("sockets::listenOrDie failed: " +
                    std::string(std::strerror(errno)));
        std::abort();
    }
}

int accept(int sockfd, InetAddress* peer_addr)
{
    struct sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    socklen_t len = sizeof(ss);
    int connfd = ::accept4(sockfd, reinterpret_cast<struct sockaddr*>(&ss),
                           &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0 && peer_addr != nullptr)
    {
        peer_addr->setSockAddrStorage(ss, len);
    }
    return connfd;
}

int connect(int sockfd, const InetAddress& addr) {
    return ::connect(sockfd, addr.getSockAddr(), addr.getSockLen());
}

void close(int sockfd)
{
    if (::close(sockfd) < 0 )
    {
        log().error("sockets::close failed: " +
                    std::string(std::strerror(errno)));
    }
}

void shutdownWrite(int sockfd)
{
    if (::shutdown(sockfd, SHUT_WR) < 0)
    {
        log().error("sockets::shutdownWrite failed: " +
                    std::string(std::strerror(errno)));
    }
}
namespace {
void setOption(int sockfd, int level, int optname, bool on, const char* what)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd, level, optname, &optval, sizeof(optval)) < 0)
    {
        log().error(std::string("sockets::setOption ") + what + " failed: " +
                    std::string(std::strerror(errno)));
    }
}
} // namespace

void setReuseAddr(int sockfd, bool on)
{
    setOption(sockfd, SOL_SOCKET, SO_REUSEADDR, on, "SO_REUSEADDR");
}

void setReusePort(int sockfd, bool on)
{
#ifdef SO_REUSEPORT
    setOption(sockfd, SOL_SOCKET, SO_REUSEPORT, on, "SO_REUSEPORT");
#else
    if (on)
    {
        log().error("sockets::setReusePort: SO_REUSEPORT not supported");
    }
#endif
}
void setTcpNoDelay(int sockfd, bool on)
{
    setOption(sockfd, IPPROTO_TCP, TCP_NODELAY, on, "TCP_NODELAY");
}

void setKeepAlive(int sockfd, bool on)
{
    setOption(sockfd, SOL_SOCKET, SO_KEEPALIVE, on, "SO_KEEPALIVE");
}

int getSocketError(int sockfd) {
    int optval;
    socklen_t optlen = sizeof(optval);
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0 ) {
        return errno;
    }
    return optval;
}

InetAddress getLocalAddr(int sockfd)
{
    struct sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));

    socklen_t len = sizeof(ss);
    if (::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&ss), &len) < 0) {
        log().error("sockets::getLocalAddr failed: " +
                    std::string(std::strerror(errno)));
    }
    InetAddress addr;
    addr.setSockAddrStorage(ss, len);
    return addr;
}

InetAddress getPeerAddr(int sockfd)
{
    struct sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    socklen_t len = sizeof(ss);
    if (::getpeername(sockfd, reinterpret_cast<struct sockaddr*>(&ss), &len) < 0 )
    {
        log().error("sockets::getPeerAddr failed: " + 
                    std::string(std::strerror(errno)));
    }
    InetAddress addr;
    addr.setSockAddrStorage(ss, len);
    return addr;
}

bool isSelfConnect(int sockfd) {
    InetAddress local = getLocalAddr(sockfd);
    InetAddress peer = getPeerAddr(sockfd);
    if (local.family() != peer.family())
    {
        return false;
    }
    if (local.isUnix())
    {
        return false;   // Unix sockets don't self-connect in this sense
    }

    return local.port() == peer.port() && local.toIp() == peer.toIp();
}

} // namespace sockets

Socket::~Socket() { reset(); }

void Socket::reset()
{
    if (sockfd_ >= 0)
    {
        sockets::close(sockfd_);
        sockfd_ = -1;
    }
}
} // namespace net


