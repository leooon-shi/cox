#pragma once

#include <unordered_map>
#include <vector>

namespace reactor {
class Channel;
class Poller {
public:
    virtual ~Poller() = default;

    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;
    virtual void poll(int timeoutMs, std::vector<Channel*>& activeChannels) = 0;

protected:
    std::unordered_map<int, Channel*> channels_;
};

} // namespace reactor