#include "tcp_connection.h"

#include "event_loop.h"
#include "logger.h"

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
}   // namespace

TcpConnection::TcpConnection(reactor::EventLoop* loop, std::string name,
                             int sockfd, const InetAddress& local,
                             const InetAddress& peer)
    : loop_(loop),
    name_(std::move(name)),
    state_(State::kConnecting),
    socket_(sockfd),
    channel_(sockfd, loop),
    local_addr_(local),
    peer_addr_(peer)
{
    channel_.setReadCallback([this] { handleRead(); });
    channel_.setWriteCallback([this] { handleWrite(); });
    channel_.setErrorCallback([this] { handleError(); });
    socket_.setKeepAlive(true);
}

TcpConnection::~TcpConnection()
{
    log().debug("TcpConnection::dtor [" + name_ + "] state=" +
               std::string(stateToString()));
}

const char* TcpConnection::stateToString() const {
    switch (state_.load())
    {
        case State::kConnecting: return "kConnecting";
        case State::kConnected: return "kConnected";
        case State::kDisconnecting: return "kDisconnecting";
        case State::kDisconnected: return "kDisconnected";
    }
    return "unknown";
}

void TcpConnection::connectEstablished()
{
    loop_->assertInLoopThread();
    setState(State::kConnected);
    // Tie the channel to this connection so it survives its own callbacks.
    channel_.tie(shared_from_this());
    channel_.enableRead();
    if (connection_callback_)
    {
        connection_callback_(shared_from_this());
    }
    // Arm the idle timeout if the connection callback configured one.
    resetIdleTimer();
    // Arm the connection max-age timer (fires once, regardless of activity).
    armMaxAgeTimer();
}

void TcpConnection::connectDestroyed()
{
    loop_->assertInLoopThread();
    // Cancel any armed timers FIRST. Each timer callback captures
    // shared_from_this() to stay valid until it fires, so an armed timer keeps
    // this connection (and its input/output Buffers) alive indefinitely when the
    // loop is stopping and the timer will never run. handleClose() alreay does
    // this; connectDestroyed() is the other teardown path (client/server
    // shutdown) and was missing it, which LeakSanitizer reported as an
    // unreclaimed TcpConnection.
    cancelIdleTimer();
    cancelWriteTimer();
    if (max_age_timer_armed_)
    {
        loop_->cancel(max_age_timer_);
        max_age_timer_armed_ = false;
    }

    if (state_ == State::kConnected)
    {
        setState(State::kDisconnected);
        channel_.disableAll();
        if (connection_callback_)
        {
            connection_callback_(shared_from_this());
        }
    }

    channel_.remove();
}

void TcpConnection::handleRead()
{
    loop_->assertInLoopThread();
    int saved_errno = 0;
    ssize_t n = input_buffer_.readFd(channel_.fd(), &saved_errno);
    if (n > 0)
    {
        resetIdleTimer();  // activity: push the idle deadline forward
        if (message_callback_)
        {
            message_callback_(shared_from_this(), &input_buffer_);
        }
        // Enforce the input-buffer cap AFTER the app had a chance to consume.
        // If data still exceeds the limit, the peer is flooding faster than we
        // consume (or sent an oversized/unframed message): force-close to bound
        // read-side memory.
        if (max_input_buffer_size_ > 0 &&
            input_buffer_.readableBytes() > max_input_buffer_size_)
        {
            log().warning("TcpConnection [" + name_ +
                          "] input buffer exceeded " +
                          std::to_string(max_input_buffer_size_) +
                          " bytes; closing");
            forceClose();
        }
    }
    else if (n == 0)
    {
        // Peer closed the connection.
        handleClose();
    } else {
        errno = saved_errno;
        log().error("TcpConnection::handleRead [" + name_ + "] " +
                    std::string(std::strerror(errno)));
        handleError();
    }
}

void TcpConnection::resetIdleTimer() {
    loop_->assertInLoopThread();
    if (idle_seconds_ == 0 || state_ != State::kConnected)
    {
        return;
    }
    cancelIdleTimer();
    // shared_from_this keeps the connection alive until the timer fires or is
    // cancelled; a fired timer that finds the connection already closed is a
    // no-op via forceClose's state guard.
    auto self = shared_from_this();
    idle_timer_ = loop_->runAfter(
        static_cast<uint64_t>(idle_seconds_) * 1000, [self]() {
            log().warning("TcpConnection [" + self->name_ +
                          "] idle timeout; closing");
            self->forceClose();
        });
    idle_timer_armed_ = true;
}

void TcpConnection::cancelIdleTimer() {
    loop_->assertInLoopThread();
    if (idle_timer_armed_)
    {
        loop_->cancel(idle_timer_);
        idle_timer_armed_ = false;
    }
}

void TcpConnection::armWriteTimer()
{
    loop_->assertInLoopThread();
    if (write_seconds_ == 0 || write_timer_armed_ ||
        state_ != State::kConnected)
    {
        return;
    }
    auto self = shared_from_this();
    write_timer_ = loop_->runAfter(
        static_cast<uint64_t>(write_seconds_) * 1000, [self]() {
            self->write_timer_armed_ = false;
            // Still backed up after the deadline -> slow consumer; force-close.
            if (self->output_buffer_.readableBytes() > 0)
            {
                log().warning("TcpConnection [" + self->name_ +
                              "] write timeout (slow consumer); closing");
                self->forceClose();
            }
        });
    write_timer_armed_ = true;
}

void TcpConnection::cancelWriteTimer()
{
    loop_->assertInLoopThread();
    if (write_timer_armed_)
    {
        loop_->cancel(write_timer_);
        write_timer_armed_ = false;
    }
}

void TcpConnection::armMaxAgeTimer()
{
    loop_->assertInLoopThread();
    if (max_age_seconds_ == 0 || max_age_timer_armed_)
    {
        return;
    }
    auto self = shared_from_this();
    max_age_timer_ = loop_->runAfter(
        static_cast<uint64_t>(max_age_seconds_) * 1000, [self]() {
            self->max_age_timer_armed_ = false;
            log().info("TcpConnection [" + self->name_ +
                       "] max age reached; closing");
            self->forceClose();
        });
    max_age_timer_armed_ = true;
}

void TcpConnection::handleWrite()
{
    loop_->assertInLoopThread();
    if (!channel_.isWriting())
    {
        return;  // write already disabled; nothing to do.
    }
    ssize_t n = ::write(channel_.fd(), output_buffer_.peek(),
                        output_buffer_.readableBytes());
    if (n > 0)
    {
        output_buffer_.retrieve(static_cast<size_t>(n));
        if (output_buffer_.readableBytes() == 0)
        {
            // All buffered output flushed: stop watching for writability.
            channel_.disableWrite();
            cancelWriteTimer();  // output drained: no longer slow
            if (write_complete_callback_)
            {
                write_complete_callback_(shared_from_this());
            }
            if (state_ == State::kDisconnecting)
            {
                shutdownInLoop();  // complete a pending half-close
            }
        }
    } else {
        log().error("TcpConnection::handleWrite [" + name_ + "] " +
                    std::string(std::strerror(errno)));
    }
}

void TcpConnection::handleClose()
{
    loop_->assertInLoopThread();
    setState(State::kDisconnected);
    cancelIdleTimer();
    cancelWriteTimer();
    if (max_age_timer_armed_)
    {
        loop_->cancel(max_age_timer_);
        max_age_timer_armed_ = false;
    }
    channel_.disableAll();

    TcpConnectionPtr guard(shared_from_this());
    if (connection_callback_)
    {
        connection_callback_(guard);   // notify app of disconnect
    }

    if(close_callback_)
    {
        close_callback_(guard);   // notify owner (TcpServer) to remove us
    }
}

void TcpConnection::handleError()
{
    int err = sockets::getSocketError(channel_.fd());
    // A hangup (EPOLLHUP) surfaces as an error revent with no pending SO_ERROR.
    // Treat that as a normal close rather than logging a misleading error.
    if (err != 0)
    {
        log().error("TcpConnection::handleError [" + name_ + "] SO_ERROR=" +
                    std::to_string(err) + " " + std::string(std::strerror(err)));
    }
    handleClose();
}

void TcpConnection::send(const std::string& message)
{
    send(message.data(), message.size());
}

void TcpConnection::send(const char* data, size_t len)
{
    if (state_ != State::kConnected)
    {
        return;
    }
    if (loop_->isInLoopThread())
    {
        sendInLoop(data, len);
    }
    else
    {
        // Copy the data since the caller's buffer may not outlive the async op.
        std::string copy(data, len);
        loop_->runInLoop([self = shared_from_this(), copy = std::move(copy)]() {
            self->sendInLoop(copy.data(), copy.size());
        });
    }
}

void TcpConnection::sendInLoop(const char* data, size_t len)
{
    loop_->assertInLoopThread();
    if (state_ == State::kDisconnected)
    {
        return;
    }

    ssize_t nwrote = 0;
    size_t remaining = len;

    // If nothing is queued, try writing directly first (fast path).
    if (!channel_.isWriting() && output_buffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_.fd(), data, len);
        if (nwrote >= 0)
        {
            remaining = len - static_cast<size_t>(nwrote);
            if (remaining == 0 && write_complete_callback_)
            {
                write_complete_callback_(shared_from_this());
            }
        }
        else
        {
            nwrote = 0;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                log().error("TcpConnection::sendInLoop [" + name_ + "] " +
                            std::string(std::strerror(errno)));
            }
        }
    }

    // Buffer whatever couldn't be written immediately and watch for writability.
    if (remaining > 0)
    {
        const size_t old_len = output_buffer_.readableBytes();
        // Fire the high-water-mark callback on the upward crossing so the app
        // can apply backpressure. Only on the transition, not repeatedly.
        if (old_len < high_water_mark_ &&
            old_len + remaining >= high_water_mark_ &&
            high_water_mark_callback_) {
            loop_->runInLoop(
                [self = shared_from_this(), n = old_len + remaining]() {
                    self->high_water_mark_callback_(self, n);
                });
        }

        output_buffer_.append(data + nwrote, remaining);
        if (!channel_.isWriting())
        {
            channel_.enableWrite();
        }
        // Output has backed up (peer isn't draining fast enough): arm the
        // write-timeout watchdog. It is cancelled in handleWrite once the
        // buffer fully drains.
        armWriteTimer();
    }
}

size_t TcpConnection::outputBufferPending() const
{
    return output_buffer_.readableBytes();
}

void TcpConnection::shutdown()
{
    if (state_ == State::kConnected)
    {
        setState(State::kDisconnecting);
        loop_->runInLoop(
            [self = shared_from_this()]() { self->shutdownInLoop(); });
    }
}

void TcpConnection::shutdownInLoop()
{
    loop_->assertInLoopThread();
    if (!channel_.isWriting())
    {
        // Output already drained: safe to half-close now.
        socket_.shutdownWrite();
    }
    // Otherwise handleWrite() will call shutdownInLoop() again once drained.
}

void TcpConnection::forceClose()
{
    if (state_ == State::kConnected || state_ == State::kDisconnecting)
    {
        setState(State::kDisconnecting);
        loop_->runInLoop([self = shared_from_this()]() { self->forceCloseInLoop(); });
    }
}

void TcpConnection::forceCloseInLoop()
{
    loop_->assertInLoopThread();
    if (state_ != State::kDisconnected)
    {
        handleClose();
    }
}

void TcpConnection::stopRead()
{
    loop_->runInLoop([self = shared_from_this()]() { self->stopReadInLoop(); });
}

void TcpConnection::startRead()
{
    loop_->runInLoop([self = shared_from_this()]() { self->startReadInLoop(); });
}

void TcpConnection::stopReadInLoop()
{
    loop_->assertInLoopThread();
    if (reading_ && channel_.isReading())
    {
        // Disable read interest: the kernel receive buffer fills, the TCP
        // window closes, and the peer's writes are throttled (no data lost).
        channel_.disableRead();
        reading_ = false;
    }
}

void TcpConnection::startReadInLoop()
{
    loop_->assertInLoopThread();
    if (!reading_ && state_ == State::kConnected)
    {
        channel_.enableRead();
        reading_ = true;
    }
}

}   // namespace net
