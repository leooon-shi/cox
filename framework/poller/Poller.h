
#pragma once
#include <map>
#include <memory>

namespace framework {

class Reactor;
class Channel
class Poller {
public:
    using Channels = std::vector<Channel*>;
    // enum class Type {
    //     EPOLL_ET,
    //     EPOLL_LT,
    //     POLL
    // };
    virtal ~Poller() = 0;
    /// @param chan A event channel manage file descriptor and interested events.
    /// The amount of kernel memory consumed by epoll is determined by 
    /// /proc/sys/fs/epoll/max_user_watches, 1/25 (%4) of the available low memory,
    /// divided by the registeration cost in bytes.
    virtual void updateEventHandler(Channel* chan) = 0;
    virtual void removeEventHandler(Channel* chan) = 0;
    virtual void poll(int tmoMs, Channels * readyChans) = 0;

    bool hasChannel(Channel* eventHandler);

    static std::unique_ptr<Poller> defaultPoller();
protected:
    // Key is file descriptor, and the value is its corresponding Channel.
    using ChannelMap = std::map<int, Channel>;
    /// @Size of eventHandlers_, 
    /// Each registered file descriptor costs roughly 90 bytes on 32-bit kernel
    /// , and roughly 160bytes on 64-bit kernel.
    ChannelMap eventHandlers_;

private:
    Reactor* eventLoop_;
};

}