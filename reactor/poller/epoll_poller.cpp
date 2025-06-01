#if defined(__linux__)
#include "epoll_poller.h"
#include "io_channel.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>

namespace Reactor {
EPollPoller::EPollPoller() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        std::cerr << "Failed to create epoll fd\n";
        std::abort();
    }
    events_.resize(64);
}

EPollPoller::~EPollPoller() {
    ::close(epoll_fd_);
}

void EPollPoller::poll(int timeout_ms, std::vector<Channel*>* active_channels) {
    int num = ::epoll_wait(epoll_fd_, events_.data(), events_.size(), timeout_ms);
    if (num < 0) {
        if (errno != EINTR) std::perror("epoll_wait");
        return;
    }

    for (int i = 0; i < num; ++i) {
        Channel* ch = static_cast<Channel*>(events_[i].data.ptr);
        ch->setRevents(events_[i].events);
        active_channels->push_back(ch);
    }

    if (num == static_cast<int>(events_.size())) {
        events_.resize(events_.size() * 2);  // grow for next round
    }
}

void EPollPoller::update(int op, Channel* channel) {
    epoll_event ev = {};
    ev.events = channel->events();
    ev.data.ptr = channel;

    if (::epoll_ctl(epoll_fd_, op, channel->fd(), &ev) < 0) {
        std::perror("epoll_ctl");
    }
}

void EPollPoller::updateChannel(Channel* channel) {
    update(EPOLL_CTL_MOD, channel);  // assume always mod; fallback to add
}

void EPollPoller::removeChannel(Channel* channel) {
    update(EPOLL_CTL_DEL, channel);
}
} // namspace Reactor

#endif // __linux__