#pragma once

#include <sys/epoll.h>
#include <vector>

#include "Poller.h"

namespace framework
{
class Channel;
class EpollPoller : public Poller
{

public:
    EpollPoller();
    ~EpollPoller() override;
    /// @Add or update a event channel 
    /// @param eventhandler 
    void updateEventHandler(Channel* chan)  override;
    void removeEventHandler(Channel* chan)  override;
    void poll(int tmoMs, Channels * readyChans)  override;
private:
    /// @Update This call is used to add, modify, or remove event channel
    /// in the interest list of epoll instance referred to by file descriptor
    /// epfd. 
    /// @param operation EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_DEL
    /// @param chan A event channel between epoll and concrete application.
    /// @return error code return by the function epoll_ctl
    int update(int operation, Channel* chan);

    static constexpr int MAX_EPOLL_EVENtS = 16; //FIXME why 16

    using Events = std::vector<struct epoll_event>;
    int epfd_;
    Events events_; // events is used to return information from the ready list about fds in the interest list that have some events available
};

}