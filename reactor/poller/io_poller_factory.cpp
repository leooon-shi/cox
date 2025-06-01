#include "io_poller_factory.h"
#include "io_poller.h"

#if defined(__linux__)
#include "poller/epoll_poller.h"
#elif defined (__APPLE__) || defined(__FreeBSD__)
#include "poller/kqueue_poller.h"
#else
#error "Unspported OS"
#endif
namespace reactor {

std::unique_ptr<Poller> PollerFactory::create() {
#if defined(__linux__)
    return std::make_unique<EPollPoller>();
#elif defined(__APPLE__) || defined (__FreeBSD__)
    return std::make_unique<KQueuePoller>();
#endif
}
} //namespace reactor