#include "io_channel.h"

#include "event_loop.h"

#include <unistd.h>
#include <cassert>

namespace reactor {

Channel::Channel(int fd, EventLoop* loop)
    : fd_(fd), events_(IO_NONE), revents_(IO_NONE), loop_(loop) {
#ifdef DDEBUG
    assert(loop_ != nullptr);
#endif
}

Channel::~Channel() {}

void Channel::tie(const std::shared_ptr<void>& owner) {
    tie_ = owner;
    tied_ = true;
}

void Channel::handleEvents() {
    if (tied_) {
        // Promote the owner to keep it alive for the duration of dispatch. If
        // it has already been destroyed, skip dispatch entirely.
        std::shared_ptr<void> guard = tie_.lock();
        if (guard) {
            handleEventsGuarded();
        }
    } else {
        handleEventsGuarded();
    }
}

void Channel::handleEventsGuarded() {
    if (revents_ & IO_ERROR && errorCallback_) {
        errorCallback_();
    }
    if (revents_ & IO_READ && readCallback_) {
        readCallback_();
    }
    if (revents_ & IO_WRITE && writeCallback_) {
        writeCallback_();
    }
}

void Channel::update() {
    assert(loop_ != nullptr);
    assert(fd_ >= 0);
    assert(loop_->isInLoopThread());

    loop_->updateChannel(this);
}

void Channel::remove() {
    assert(loop_ != nullptr);
    assert(loop_->isInLoopThread());

    loop_->removeChannel(this);
}

}  // namespace reactor
