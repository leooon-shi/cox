#include "connector.h"

#include "event_loop.h"
#include "io_channel.h"
#include "logger.h"
#include "net_runtime.h"
#include "socket.h"

#include <cerrno>
#include <cstring>
#include <string>

namespace net {
namespace {
logging::Logger& log() {
    static logging::Logger logger;
    return logger;
}
}  // namespace

namespace {
// Build a non-empty candidate vector from a single address (single-address
// ctor delegates here).
std::vector<InetAddress> single(const InetAddress& a) {
    return std::vector<InetAddress>{a};
}
}  // namespace

Connector::Connector(reactor::EventLoop* loop, const InetAddress& server_addr)
    : Connector(loop, single(server_addr)) {}

Connector::Connector(reactor::EventLoop* loop,
                     std::vector<InetAddress> candidates)
    : loop_(loop),
      candidates_(std::move(candidates)),
      // Key backoff by the first candidate's endpoint identity: cycling
      // through the whole list is one logical failure, not one per address.
      backoff_key_(candidates_.empty() ? std::string()
                                       : candidates_.front().toIpPort()),
      state_(State::kDisconnected),
      backoff_(std::chrono::milliseconds(kInitRetryDelayMs),
               std::chrono::milliseconds(kMaxRetryDelayMs),
               /*jitter=*/0.5) {
    initNetRuntime();
}

Connector::~Connector() {
    // The channel must already have been reset (connect completed or stopped).
}

void Connector::start() {
    connect_ = true;
    loop_->runInLoop([self = shared_from_this()]() { self->startInLoop(); });
}

void Connector::startInLoop() {
    loop_->assertInLoopThread();
    if (connect_) {
        connect();
    }
}

void Connector::stop() {
    connect_ = false;
    loop_->runInLoop([self = shared_from_this()]() { self->stopInLoop(); });
}

void Connector::stopInLoop() {
    loop_->assertInLoopThread();
    cancelConnectTimeout();
    if (retry_timer_armed_) {
        loop_->cancel(retry_timer_);
        retry_timer_armed_ = false;
    }

    if (state_ == State::kConnecting) {
        int sockfd = removeAndResetChannel();
        sockets::close(sockfd);
        setState(State::kDisconnected);
    }
}

void Connector::restart() {
    loop_->assertInLoopThread();
    setState(State::kDisconnected);
    backoff_.reset(backoffKey());  // back to initial delay
    current_ = 0;                  // restart the candidate walk from the top
    connect_ = true;
    startInLoop();
}

void Connector::connect() {
    const InetAddress& target = candidates_[current_];
    int sockfd = sockets::createNonBlockingOrDie(target.family());
    int ret = sockets::connect(sockfd, target);
    int saved_errno = (ret == 0) ? 0 : errno;
    switch (saved_errno) {
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
        case EAFNOSUPPORT:  // family unreachable: fail over to the next
                            // candidate
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

void Connector::connecting(int sockfd) {
    setState(State::kConnecting);
    channel_ = std::make_unique<reactor::Channel>(sockfd, loop_);
    channel_->setWriteCallback([this] { handleWrite(); });
    channel_->setErrorCallback([this] { handleError(); });
    channel_->enableWrite();
    armConnectTimeout();
}

int Connector::removeAndResetChannel() {
    channel_->disableAll();
    channel_->remove();
    int sockfd = channel_->fd();
    // Destroy the channel on a LATER loop iteration: we are currently inside
    // its own callback, so freeing it now would leave Channel::handleEvents
    // touching freed memory when the callback returns.
    // NOTE: this must be queueInLoop, not runInLoop -- runInLoop runs the task
    // INLINE when already on the loop thread, which is exactly the case here,
    // so it did not defer anything at all. ThreadSanitizer caught the resulting
    // heap-use-after-free.
    loop_->queueInLoop([self = shared_from_this()]() { self->resetChannel(); });

    return sockfd;
}

void Connector::resetChannel() {
    channel_.reset();
}

void Connector::handleWrite() {
    if (state_ != State::kConnecting) {
        return;
    }
    cancelConnectTimeout();  // the attempt has resolved (success or error)

    int sockfd = removeAndResetChannel();
    int err = sockets::getSocketError(sockfd);
    if (err != 0) {
        log().warning("Connector::handleWrite SO_ERROR=" + std::to_string(err) +
                      " " + std::string(std::strerror(err)));
        retry(sockfd);
    } else if (sockets::isSelfConnect(sockfd)) {
        log().warning("Connector::handleWrite self-connect detected; retrying");
        retry(sockfd);
    } else {
        setState(State::kConnected);
        if (connect_) {
            if (new_connection_callback_) {
                new_connection_callback_(sockfd);
            } else {
                sockets::close(sockfd);
            }
        } else {
            sockets::close(sockfd);
        }
    }
}

void Connector::handleError() {
    log().error("Connector::handleError state=" +
                std::to_string(static_cast<int>(state_.load())));
    if (state_ == State::kConnecting) {
        cancelConnectTimeout();
        int sockfd = removeAndResetChannel();
        int err = sockets::getSocketError(sockfd);
        log().error("Connector SO_ERROR=" + std::to_string(err));
        retry(sockfd);
    }
}

void Connector::armConnectTimeout() {
    connect_timer_ =
        loop_->runAfter(connect_timeout_ms_, [self = shared_from_this()]() {
            self->connect_timer_armed_ = false;
            self->onConnectTimeout();
        });
    connect_timer_armed_ = true;
}

void Connector::cancelConnectTimeout() {
    if (connect_timer_armed_) {
        loop_->cancel(connect_timer_);
        connect_timer_armed_ = false;
    }
}

void Connector::onConnectTimeout() {
    loop_->assertInLoopThread();
    // The attempt to the current candidate neither completed nor errored in
    // time (e.g. a blackholed address). Abandon it and fail over.
    if (state_ != State::kConnecting) {
        return;
    }
    log().warning("Connector::onConnectTimeout to " +
                  candidates_[current_].toIpPort() + " after " +
                  std::to_string(connect_timeout_ms_) + " ms");
    int sockfd = removeAndResetChannel();
    retry(sockfd);
}
void Connector::retry(int sockfd) {
    sockets::close(sockfd);
    setState(State::kDisconnected);
    if (!connect_) {
        return;
    }

    // Dual-stack failover: if another candidate remains, try it IMMEDIATELY
    // (no backoff) -- backoff is for a down endpoint, not for stepping between
    // a host's addresses (e.g. ::1 refused -> try 127.0.0.1 now).
    //
    // The next connect() reassigns channel_, which frees the current Channel.
    // We are presently executing INSIDE that Channel's event callback (retry()
    // is reached from handleWrite()/handleError()/onConnectTimeout()), so
    // freeing it now is a use-after-free when handleEvents() returns -- the
    // same hazard removeAndResetChannel() guards against. Defer the attempt to
    // the next loop iteration with queueInLoop: still "immediate" (no timer
    // delay) but after the current callback unwinds and the old Channel is
    // reset. (ThreadSanitizer/ASan caught the synchronous version.)
    if (current_ + 1 < candidates_.size()) {
        ++current_;
        log().info("Connector::retry failing over to " +
                   candidates_[current_].toIpPort());
        loop_->queueInLoop([self = shared_from_this()]() {
            if (self->connect_) self->connect();
        });
        return;
    }

    // Candidate list exhausted: the endpoint is (currently) unreachable. Reset
    // to the first candidate, advance the jittered exponential backoff for this
    // endpoint, and retry the whole list after the delay. Jitter decorrelates
    // mass reconnects.
    current_ = 0;
    auto delay = backoff_.nextDelay(backoffKey());
    log().info("Connector::retry to " + backoffKey() + " in " +
               std::to_string(delay.count()) + " ms");
    retry_timer_ = loop_->runAfter(static_cast<uint64_t>(delay.count()),
                                   [self = shared_from_this()]() {
                                       self->retry_timer_armed_ = false;
                                       if (self->connect_) {
                                           self->connect();
                                       }
                                   });
    retry_timer_armed_ = true;
}

}  // namespace net
