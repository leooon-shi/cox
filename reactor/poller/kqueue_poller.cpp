#include "kqueue_poller.h"
#include "io_channel.h"
#include <unistd.h>
#include <stdexcept>
#include  <fcntl.h>
#include "logger.h"
#include <err.h>

namespace reactor {
namespace {
    void setCLOEXEC(int fd)
    {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
}
KQueuePoller::KQueuePoller()
#if defined(__APPLE__)
: kqueueFd_(::kqueue()),
#else
: kqueueFd_(::kqueuex(KQUEUE_CLOEXEC)),
#endif
 events_(64),
 logger_(std::make_unique<logging::Logger>()) {
    if (kqueueFd_ == -1)
    {
        throw std::runtime_error("Failed to create kqueue");
    }
#if defined(__APPLE__)
    setCLOEXEC(kqueueFd_);
#endif
}

KQueuePoller::~KQueuePoller() {
    ::close(kqueueFd_);
}

void KQueuePoller::updateChannel(Channel* ch) {
    if (channels_.contains(ch->fd()))
    {
        logger_->info("update Channel.");
    }
    else
    {
        logger_->info("add Channel.");
    }
    // std::vector<struct kevent> kev;
    const int fd = ch->fd();
    const int ev = ch->events();
    const int reg_ev = registeredEvents[fd];
    {
        struct kevent e_read;
        
        if (ev & IO_READ) {
            // logger_->info("register EVFILT_READ with options EV_ADD|EV_ENABLE to kpoller");
            EV_SET(&e_read, fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, ch);
            if (::kevent(kqueueFd_, &e_read, 1, nullptr, 0, nullptr) == -1)
            {
                logger_->error("update kqueue got error.");
                err(EXIT_FAILURE, "update kevent failed.");
            }
        } else if (reg_ev & IO_READ) {
            EV_SET(&e_read, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            if (::kevent(kqueueFd_, &e_read, 1, nullptr, 0, nullptr) == -1)
            {
                logger_->error("update kqueue got error.");
                err(EXIT_FAILURE, "update kevent failed.");
            }
        }
        // kev.push_back(e_read);
        
        // logger_->info("finished register read");
    }
    {
        struct kevent e_write;
        if (ev & IO_WRITE) {
            logger_->info("register EVFILT_READ with options EV_ADD|EV_ENABLE to kpoller");
            EV_SET(&e_write, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, ch);
            if (::kevent(kqueueFd_, &e_write, 1, nullptr, 0, nullptr) == -1)
            {
                logger_->error("update kqueue got error.");
                err(EXIT_FAILURE, "update kevent failed.");
            }
        } else  if (reg_ev & IO_WRITE)  {
            EV_SET(&e_write, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            if (::kevent(kqueueFd_, &e_write, 1, nullptr, 0, nullptr) == -1)
            {
                logger_->error("update kqueue got error.");
                err(EXIT_FAILURE, "update kevent failed.");
            }
        }

        // logger_->info("finished register write");
        // kev.push_back(e);
    }

    channels_[fd] = ch;
    registeredEvents[fd] = ev;
}

void KQueuePoller::removeChannel(Channel* ch) {
    int fd = ch->fd();
    std::vector<struct kevent> kev;

    struct kevent e1;
    EV_SET(&e1, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kev.push_back(e1);

    struct kevent e2;
    EV_SET(&e2, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kev.push_back(e2);

    ::kevent(kqueueFd_, kev.data(), kev.size(), nullptr, 0, nullptr);
    channels_.erase(fd);
    registeredEvents.erase(fd);
}

void KQueuePoller::poll(int timeout_ms, std::vector<Channel*> & activeChannels) {
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;

    int n = ::kevent(kqueueFd_, nullptr, 0, events_.data(), events_.size(), &ts);
    if (n <= 0)
    {
        //TODO Handle ERROR.
    }

    for (int i = 0; i < n; ++i) {
        auto& e = events_[i];
        Channel* ch = static_cast<Channel*>(e.udata);
        if (!ch) continue;

        IOEvent rev = IO_NONE;
        if (e.filter == EVFILT_READ)  rev = IO_READ;
        if (e.filter == EVFILT_WRITE) rev = IO_WRITE;
        if (e.flags & EV_ERROR)       rev = rev | IO_ERROR;

        ch->setRevents(rev);
        activeChannels.push_back(ch);
    }
}

} // namespace reactor