#include "Poller.h"
#include "EpollPoller.h"

namespace framework
{

std::unique_ptr<Poller> Poller::defaultPoller()
{
    return std::make_unique<EpollPoller>();
}
}