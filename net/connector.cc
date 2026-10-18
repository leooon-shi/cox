#include "connector.h"

#include "event_loop.h"
#include "io_channel.h"
#include "logger.h"
#include "net_runtime.h"
#include "socket.h"

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
} // namespace

Connector::Connector( reactor::EventLoop* loop, const InetAddress& server_addr)
: loop_(loop),
  server_addr_(server_addr),
  state_(State::kDisconnected),
  backoff_(std::chrono::milliseconds(kInitRetryDelayMs),
            std::chrono::milliseconds(kMaxRetryDelayMs),
            /*jitter*/0.5)
{
    initNetRuntime();
}

Connector::~Connector()
{
    //The channel must already have been reset (connect completed or stopped).
}

void Connector::start()
{
    connect_ = true;
    loop_->runInLoop([self = shared_from_this()](){ self->startInLoop(); });
}

void Connector::startInLoop()
{
    loop_->assertInLoopThread();
    if (connect_)
    {
        connect();
    }
}

void Connector::stop()
{
    connect_ = false;
    loop_->runInLoop([self = shared_from_this()](){ self->stopInLoop(); });
}

void Connector::stopInLoop()
{
    loop_->assertInLoopThread();
    if (retry_timer_armed_)
    {
        loop_->cancel(retry_timer_);
        retry_timer_armed_ = false;
    }

    if (state_ == State::kConnecting)
    {
        int sockfd = removeAndResetChannel();
        sockets::close(sockfd);
        setState(State::kDisconnected);
    }
}

void Connector::restart()
{
    loop_->assertInLoopThread();
    setState(State::kDisconnected);
    backoff_.reset(server_addr_.toIpPort());    // Back to initial delay
    connect_ = true;
    startInLoop();
}

void Connector::connect()
{
    int sockfd = sockets::createNonBlockingOrDie(server_addr_.family());
    int ret = sockets::connect(sockfd, server_addr_);
    int saved_errno = (ret == 0) ? 0 : errno;
    switch(saved_errno)
    {
        case 0:
        case EINPROGRESS:
        case EINTR:
        case EISCONN:
            connecting(sockfd);
            break;
        case EAGAIN:
        case EADDRINUSE:
        case EADDRNOTAVAIL:
        case ECONNREFUSED:
        case ENETUNREACH:
        // Unix-domain: the socket path may not exist yet (server not up) or
        // the directory may be transiently unavailable. Treat as retryable so
        // a client started before its server can reconnect.
        case ENOENT:
        case ECONNRESET:
            retry(sockfd);
            break;
        default:
            log().error("Connector::connect unexpected error " +
                        std::to_string(saved_errno) + " " +
                        std::string(std::strerror(saved_errno)));
            sockets::close(sockfd);
            break;
    }
}

void Connector::connecting(int sockfd)
{
    setState(State::kConected);
    channel_ = std::make_unique<reactor::Channel>(sockfd, loop_);
    channel_->setWriteCallback([this]{ handleWrite(); });
    channel_->setErrorCallback([this]{ handleError(); });
    channel_->enableWrite();
}

int Connector::removeAndResetChannel()
{
    channel_->disableAll();
    channel_->remove();
    int sockfd = channel_->fd();
    // Destroy the channel on a LATER loop iteration: we are currently inside
    // its own callback, so freeing it now would leave Channel::handleEvents
    // touching freed memory when the callback returns.
    // NOTE: this must be queueInLoop, not runInLoop -- runInLoop runs the task
    // INLINE when already on the loop thread, which is exactly the case here.
    // so it did not defer anything at all. ThreadSanitizer caught the resulting
    // heap-use-after-free.
    loop_->queueInLoop([self = shared_from_this()](){ self->resetChannel(); });

    return sockfd;
}

void Connector::resetChannel() 
{ 
    channel_.reset(); 
}

void Connector::handleWrite()
{
    if (state_ != State::kConnecting)
    {
        return;
    }

    int sockfd = removeAndResetChannel();
    int err = sockets::getSocketError(sockfd);
    if (err != 0)
    {
        log().warning("Connector::handleWrite SO_ERROR=" + std::to_string(err) +
                        " " + std::string(std::strerror(err)));
        retry(sockfd);
    }
    else
    {
        setState(State::kConected);
        if (connect_)
        {
            if (new_connection_callback_)
            {
                new_connection_callback_(sockfd);
            }
            else
            {
                sockets::close(sockfd);
            }
        }
        else
        {
            sockets::close(sockfd);
        }
    }
}

void Connector::handleError()
{
    log().error("Connector::handleError state=" + 
                std::to_string(static_cast<int>(state_.load())));
    if (state_ == State::kConnecting)
    {
        int sockfd = removeAndResetChannel();
        int err = sockets::getSocketError(sockfd);
        log().error("Connector SO_ERROR=" + std::to_string(err));
        retry(sockfd);
    }
}

void Connector::retry(int sockfd)
{
    sockets::close(sockfd);
    setState(State::kDisconnected);
    if (!connect_)
    {
        return;
    }
    // Advance the jittered exponential backoff for this endpoint and wait that
    // long before the next attempt, jitter decorrelates mass reconnects.
    auto delay = backoff_.nextDelay(server_addr_.toIpPort());
    log().info("Connector::retry to " + server_addr_.toIpPort() + " in " + 
                std::to_string(delay.count()) + "ms");
    retry_timer_armed_ = true;
}

} // namespace net