#include <sys/epoll.h>

#include "EpollPoller.h"
#include "Channel.h"

static_assert(EPOLLIN == POLLIN, "EPOLLIN is not equal to POLLIN.");
static_assert(EPOLLPRI == POLLPRI, "EPOLLPRI is not equal to POLLPRI.");
static_assert(EPOLLOUT == POLLOUT, "EPOLLOUT is not equal to POLLOUT.");
static_assert(EPOLLERR == POLLERR, "EPOLLERR is not equal to POLLERR.");
static_assert(EPOLLHUP == POLLHUP, "EPOLLHUP is not equal to POLLHUP.");
//since Linux 2.6.17
static_assert(EPOLLRDHUP == POLLRDHUP, "EPOLLIN is not equal to POLLRDHUP.");

namespace framework
{

EpollPoller::EpollPoller()
: epfd_(epoll_create1(EPOLL_CLOEXEC)),
  events_(MAX_EPOLL_EVENtS)
{
}

EpollPoller::~EpollPoller()
{
    ::close(epfd);
}

void EpollPoller::updateEventHandler(Channel *chan)
{
    if (hasChannel(chan))
    {
        update(EPOLL_CTL_MOD, chan);
    }
    else
    {
        update(EPOLL_CTL_ADD, chan);
    }
}

void EpollPoller::removeEventHandler(Channel *chan)
{
    if(update(EPOLL_CTL_DEL, chan) != 0)
    {
        //TODO log error
    }
}

void EpollPoller::poll(int tmoMs, Channels * readyChans)
{
    int numsRevents = epoll_wait(epfd_, &(*events_.begin()), events.size(), tmoMs);
    int savedErrNo = errno;
    if (numsRevents > 0)
    {
        for (int i = 0; i < reventsNums; i++)
        {
            auto epEv = events_[i];
            auto * chan = static_cast<Channel*>(epEv.data.ptr);
            chan->setRevents(epEv.events);
            readyChans->push_back(chan);
        }
    }
    else if (numsRevents == 0)
    {
        //NOTHING HAPPENED
        //TODO TRACE IT.
    }
    else
    {
        //TODO Log ERROR
    }
}

int EpollPoller::update(int operation, Channel* chan)
{
    int savedErrNo = 0;
    struct epoll_event ev { 0 };
    ev.events = chan->events();
    ev.data.ptr = chan;

    if (epoll_ctl(epfd_, operation, chan->fd(), &ev) == -1)
    {
        savedErrNo = errno;
        //TODO log error information
    }

    return savedErrNo;
}

}