#include "io_channel.h"
#include "event_loop.h"

#include <unistd.h>
#include <cassert>

namespace reactor {
//FIXME: loop should not be nullptr.

Channel::Channel(int fd, EventLoop* loop)
    : fd_(fd), events_(IO_NONE), revents_(IO_NONE), loop_(loop), tied_(false) {
    assert(loop_ != nullptr);
}

Channel::~Channel() {
}

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
            handleEventsGuided();
        }
    } else {
        handleEventsGuided();
    }
}

void Channel::handleEventsGuided() {
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
    assert(fd_ >= 0);
    assert(loop_->isInLoopThread());

    loop_->removeChannel(this);
}

} // namespace reactor