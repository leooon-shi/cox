#inclde "Poller.h"

namespace framework {

bool Poller::hasChannel(Channel* eventHandler)
{
    return eventHandlers_.contains(eventHandler->fd());
}

}