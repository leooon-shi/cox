#pragma once

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#include "poller/io_poller.h"
#include <sys/event.h>
#include <vector>
#include <map>

namespace logging
{
    class Logger;
}
namespace reactor {
class Channel;

class KQueuePoller : public Poller {
public:
    KQueuePoller();
    ~KQueuePoller() override;

    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;
    void poll(int timeoutMs, std::vector<Channel*>& activeChannels) override;

private:
    void update(int operation, Channel* channel);

    std::unique_ptr<logging::Logger> logger_;
    int kqueueFd_;
    std::vector<struct kevent> events_;
    std::map<int, short> registeredEvents;
};

} // namespace reactor

#endif // BSD / macOS