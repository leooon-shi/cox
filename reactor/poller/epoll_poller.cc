#if defined(__linux__)
#include "epoll_poller.h"
#include "io_channel.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace reactor {

namespace {
// Translate the library's IOEvent bitmask into epoll event flags.
uint32_t toEpollEvents(int events) {
    uint32_t ep = 0;
    if (events & IO_READ) ep |= EPOLLIN;
    if (events & IO_WRITE) ep |= EPOLLOUT;
    return ep;
}

// Translate epoll event flags into the library's IOEvent bitmask.
int fromEpollEvents(uint32_t events) {
    int events = IO_NONE;
    if (ep & (EPOLLERR | EPOLLHUP)) events |= IO_ERROR;
    if (ep & (EPOLLIN | EPOLLPRI)) events |= IO_READ;
    if (ep & EPOLLOUT) events |= IO_WRITE;
    return events;
}

} // namespace

EPollPoller::EPollPoller() {
    epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        std::cerr << "Failed to create epoll fd\n";
        std::abort();
    }
    events.resize(64); // Initial size for events
}

EPollPoller::~EPollPoller() {
    ::close(epollFd_);
}

void EPollPoller::poll(int timeoutMs, std::vector<Channel*>& activeChannels) {
    int num = ::epoll_wait(epollFd_, events.data(), static_cast<int>(events.size()), timeoutMs);
    if (num < 0) {
        if (errno != EINTR) {
            std::perror("epoll_wait");
        }
        return;
    }

    for (int i = 0; i < num; ++i) {
        Channel* ch = static_cast<Channel*>(events[i].data.ptr);
        ch->setRevents(fromEpollEvents(events[i].events));
        activeChannels.push_back(ch);
    }

    // Resize the events vector if it's full
    if (static_cast<size_t>(num) == events.size()) {
        events.resize(events.size() * 2); // grow for next round
    }
}

void EPollPoller::update(int operation, Channel* channel) {
    struct epoll_event ev;
    ev.events = toEpollEvents(channel->events());
    ev.data.ptr = channel;

    if (::epoll_ctl(epollFd_, operation, channel->fd(), &ev) < 0) {
        std::perror("epoll_ctl");
    }
}

void EPollPoller::updateChannel(Channel* channel) {
    const int fd = channel->fd();
    // Register a new fd with EPOLL_CTL_ADD, otherwise modify the existing one with EPOLL_CTL_MOD
    if (channels_.find(fd) == channels_.end()) {
        channels_[fd] = channel;
        update(EPOLL_CTL_ADD, channel);
    } else {
        update(EPOLL_CTL_MOD, channel);
    }
}

void EPollPoller::removeChannel(Channel* channel) {
    const int fd = channel->fd();
    if (channels_.erase(fd) > 0) {
        update(EPOLL_CTL_DEL, channel);
    }
}

} // namespace reactor
#endif