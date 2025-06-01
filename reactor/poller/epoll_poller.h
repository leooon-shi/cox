#pragma once

#ifdef __linux__

#include "io_poller.h"
#include <sys/epoll.h>
#include <vector>

namespace reactor {
class Channel;
class EPollPoller : public Poller {
public:
    EPollPoller();
    ~EPollPoller() override;

    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;
    void poll(int timeoutMs, std::vector<Channel*>& activeChannels) override;

private:
    void update(int operation, Channel* channel);

    int epollFd_;
    std::vector<epoll_event> events_;
};

} // namespace reactor

#endif // __linux__