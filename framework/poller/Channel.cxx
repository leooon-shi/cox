#include "Channel.h"
#include "reactor/Reactor.h"

namespace framework {

void Channel::Channel(int fd, Reactor * eventLoop)
:fd_(fd),
loop_(eventLoop)
{
}

void Channel::update()
{
    loop_->updateChannel(this);
}
void Channel::handleEvents()
{
    if (revents_ & ( POLLIN | POLLPRI | POLLRDHUP) )
    {
        if (readCallback_) readCallback_();
    }
}

}