#include "acceptor.h"

#include "event_loop.h"
#include "inet_address.h"
#include "logger.h"
#include "net_runtime.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace net
{
namespace
{
logging::Logger& log()
{
    static logging::Logger logger;
    return logger;
}

int openIdleFd()
{
    return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}
}   // namespace

Acceptor::Acceptor(reactor::EventLoop* loop, const InetAddress& listen_addr,
                   bool reuse_port)
    : loop_(loop),
      accept_socket_(sockets::createNonBlockingOrDie(listen_addr.family())),
      accept_channel_(accept_socket_.fd(), loop),
      listening_(false),
      idle_fd_(openIdleFd())
{
    initNetRuntime();  // ensure SIGPIPE is ignored before any socket I/O
    accept_socket_.setReuseAddr(true);
    accept_socket_.setReusePort(reuse_port);
    accept_socket_.bindAddress(listen_addr);
    accept_channel_.setReadCallback([this] { handleRead(); });
}

Acceptor::~Acceptor()
{
    accept_channel_.disableAll();
    accept_channel_.remove();
    if (idle_fd_ >= 0)
    {
        ::close(idle_fd_);
    }
}

void Acceptor::listen()
{
    loop_->assertInLoopThread();
    listening_ = true;
    accept_socket_.listen();
    accept_channel_.enableRead();
}

void Acceptor::stopListening()
{
    loop_->assertInLoopThread();
    listening_ = false;
    accept_channel_.disableAll();
}

void Acceptor::handleRead()
{
    loop_->assertInLoopThread();
    // Drain all pending connections in one go: efficient under load and correct
    // for edge-triggered pollers.
    for (;;)
    {
        InetAddress peer;
        int connfd = accept_socket_.accept(&peer);
        if (connfd >= 0)
        {
            if (new_connection_callback_)
            {
                new_connection_callback_(connfd, peer);
            } else {
                sockets::close(connfd);
            }
            continue;
        }

        // connfd < 0: inspect errno.
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;  // no more pending connections
        }

        if (errno == EINTR)
        {
            continue;
        }

        if (errno == EMFILE || errno == ENFILE)
        {
            // Out of file descriptors. Free the reserved idle fd, accept the
            // pending connection so it doesn't wedge the readable event, close
            // it immediately, then reopen the idle fd. This prevents a busy
            // loop while signaling the peer with an immediate close.
            log().error("Acceptor::handleRead out of fds (" +
                        std::string(std::strerror(errno)) + ")");

            if (idle_fd_ >= 0)
            {
                ::close(idle_fd_);
                idle_fd_ = ::accept(accept_socket_.fd(), nullptr, nullptr);
                if (idle_fd_ >= 0)
                {
                    ::close(idle_fd_);
                }
                idle_fd_ = openIdleFd();
            }
            break;
        }
        // Other errors: log and stop this round.
        log().error("Acceptor::handleRead accept failed: " +
                    std::string(std::strerror(errno)));
        break;
    }
}

}   // namespace net
