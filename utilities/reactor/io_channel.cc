#include "io_channel.h"
#include <unistd.h>
#include <cassert>
#include "event_loop.h"

namespace reactor {
//FIXME: loop should not be nullptr.
Channel::Channel(int fd, EventLoop* loop)
:fd_(fd), events_(IO_NONE), revents_(IO_NONE),loop_(loop) {}

Channel::~Channel()
{
}

void Channel::handleEvents()
{
    if (revents_ & IO_ERROR && errorCallback_) errorCallback_();
    if (revents_ & IO_READ  && readCallback_)  readCallback_();
    if (revents_ & IO_WRITE && writeCallback_) writeCallback_();
}

void Channel::update()
{
    assert(loop_ != nullptr);
    assert(fd_ >= 0);
    assert(loop_->isInLoopThread());
    
    loop_->updateChannel(this);
}

void Channel::remove()
{
    assert(loop_ != nullptr);
    assert(loop_->isInLoopThread());
    
    loop_->removeChannel(this);
}
} //namespace reactor