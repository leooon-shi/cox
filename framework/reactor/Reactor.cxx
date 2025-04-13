#include "Reactor.h"
#include "poller/Channel.h"
#include "Poller/Poller.h"

namespace framework {

Reactor::Reactor()
:poller_(Poller::defaultPoller())
{

}
void Reactor::loop()
{
    while(!stopped_)
    {
        activeChans_.clear();
        poller_->poll(timeout_, &activeChans_);
    }
}

void Reactor::stop()
{
    stopped_ = true;
}


void Reactor::updateChannel(Channel* chan)
{
    poller->updateChannel(chan);
}

}