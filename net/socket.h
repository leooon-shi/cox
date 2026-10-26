#pragma once

#include <sys/socket.h>
#include <sys/types.h>

#include <cstdint>

namespace net {

class InetAddress;

// Thin, stateless helpers over the POSIX socket API. These mirror the pragmatic
// style of Redis/anet: small functions that do one syscall and report errors
// via return codes / errno, leaving policy to the caller. Family (AF_INET vs
// AF_INET6) is passed explicitly so the layer is dual-stack.
namespace sockets {
// Create a non-blocking, close-on-exec stream socket for the given family.
// Aborts on failure (allocation of an fd is not expected to fail in practice).
int createNonBlockingOrDie(sa_family_t family);

// Bind or abort. Logs and aborts on failure (a bind failure is a config error).
void bindOrDie(int sockfd, const InetAddress& addr);

// Listen or abort.
void listenOrDie(int sockfd);

// Accept a connection, filling peer_addr. Returns the accepted non-blocking,
// close-on-exec fd, or -1 on error (errno preserved for the caller to inspect,
// e.g. EAGAIN/EMFILE handling).
int accept(int sockfd, InetAddress* peer_addr);

// Initiate a connection. Returns 0 on immediate success, or -1 (errno set,
// typically EINPROGRESS for a non-blocking socket) otherwise.
int connect(int sockfd, const InetAddress& addr);

void close(int sockfd);

// Shut down the write half (half-close), leaving reads open.
void shutdownWrite(int sockfd);

// Common options.
void setReuseAddr(int sockfd, bool on);
void setReusePort(int sockfd, bool on);
void setTcpNoDelay(int sockfd, bool on);
void setKeepAlive(int sockfd, bool on);

// Retrieve the SO_ERROR pending on a socket (used after a non-blocking connect
// completes to distinguish success from failure).
int getSocketError(int sockfd);

// Local / peer address of a connected socket.
InetAddress getLocalAddr(int sockfd);
InetAddress getPeerAddr(int sockfd);

// True if the socket's local and peer addresses are identical (self-connect),
// which can happen with a non-blocking connect to localhost.
bool isSelfConnect(int sockfd);

}  // namespace sockets

// RAII owner of a socket fd. Move-only: an fd has unique ownership semantics.
// Closes the fd on destruction unless it has been released.
class Socket {
public:
    explicit Socket(int sockfd) : sockfd_(sockfd) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : sockfd_(other.sockfd_) {
        other.sockfd_ = -1;
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            sockfd_ = other.sockfd_;
            other.sockfd_ = -1;
        }
        return *this;
    }

    int fd() const { return sockfd_; }

    // Relinquish ownership without closing; returns the fd.
    int release() {
        int fd = sockfd_;
        sockfd_ = -1;
        return fd;
    }

    void bindAddress(const InetAddress& addr) {
        sockets::bindOrDie(sockfd_, addr);
    }
    void listen() { sockets::listenOrDie(sockfd_); }
    // Accept into peer_addr; returns accepted fd or -1 (errno set).
    int accept(InetAddress* peer_addr) {
        return sockets::accept(sockfd_, peer_addr);
    }

    void shutdownWrite() { sockets::shutdownWrite(sockfd_); }
    void setReuseAddr(bool on) { sockets::setReuseAddr(sockfd_, on); }
    void setReusePort(bool on) { sockets::setReusePort(sockfd_, on); }
    void setTcpNoDelay(bool on) { sockets::setTcpNoDelay(sockfd_, on); }
    void setKeepAlive(bool on) { sockets::setKeepAlive(sockfd_, on); }

private:
    void reset();

    int sockfd_;
};

}  // namespace net
