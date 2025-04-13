#pragma
#include <vector>
#include <memory>

#include "poller/Poller.h"

namespace Framework
{
class Channel;
class Poller;
class Reactor 
{
public:
    void loop();
    void stop();
    void updateChannel(Channel* chan);
private:
    using Channels = std::vector<Channel*>;

    constexpr int DefaultTimeoutMs = 10000; // 10 seconds TODO WHY??

    bool stopped_ {false};
    std::unique_ptr<Poller> poller_;
    int timeout_ {DefaultTimeoutMs};
    Channels activeChans_;
};

}