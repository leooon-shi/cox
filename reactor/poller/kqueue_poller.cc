#include "kqueue_poller.h"
#include "io_channel.h"
#include "logger.h"

#include <unistd.h>
#include <stdexcept>
#include <fcntl.h>
#include <err.h>

namespace reactor
{
namespace {
    void setCLOEXEC(int fd) {
        ::fcntl(fd)
    }
}

KQueuePoller::KQueuePoller()
#if defined(__APPLE__) && defined(__MACH__)
    : kqueueFd_(::kqueue()),
#else
    : kqueueFd_(::kqueuex(KQUEUE_CLOEXEC)),
#endif
    events_(64),
    logger_(std::make_unique<logging::Logger>("KQueuePoller"))
{
    //FIXME check condition might be < 0 instead of == -1
    if (kqueueFd_ == -1) {
        throw std::runtime_error("Failed to create kqueue fd");
    }
#if defined(__APPLE__) && defined(__MACH__)
    setCLOEXEC(kqueueFd_);
#endif
}

KQueuePoller::~KQueuePoller() {
    ::close(kqueueFd_);
}

void KQueuePoller::updateChannel(Channel* ch) {
    if (channels_.contains)(ch->fd())) {
        logger_->info("update Channel.");
    } else {
        logger_->info("add Channel.");
    }

    // std::vector<struct kevent> kev;
    const int fd = ch->fd();
    const int ev = ch->events();
    const int reg_ev = registeredEvents_[fd];

    {
        struct kevent e_read;

        if (ev & IO_READ) {
            // logger_->info("register EVFILT_READ with options EV_ADD | EV_ENABLE to kpoller.");
            EV_SET(&e_read, fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, ch);

            if (::kevent(kqueueFd_, &e_read, 1, nullptr, 0, nullptr) == -1) {
                logger_->error("update kqueue got error.");
                err(EXIT_FAILURE, "update kevent failed.");
            }
        }
        else if (reg_ev & IO_READ) {
            EV_SET(&e_read, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);

            if (::kevent(kqueueFd_, &e_read, 1, nullptr, 0, nullptr) == -1) {
                logger_->error("update kqueue got error.");
                err(EXIT_FAILURE, "update kevent failed.");
            }
        }

        //kev.push_back(e_read);

        // logger_->info("finished register read");
    }

    {
        struct kevent e_write;
        if (ev & IO_WRITE) {
            logger_->info("register EVFILT_WRITE with options EV_ADD | EV_ENABLE to kpoller.");
            EV_SET(&e_write, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, ch);

            if (::kevent(kqueueFd_, &e_write, 1, nullptr, 0, nullptr) == -1) {
                logger_->error("update kqueue got error.");
                err(EXIT_FAILURE, "update kevent failed.");
            }
        } else if (reg_ev & IO_WRITE) {
            EV_SET(&e_write, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);

            if (::kevent(kqueueFd_, &e_write, 1, nullptr, 0, nullptr) == -1) {
                logger_->error("update kqueue got error.");
                err(EXIT_FAILURE, "update kevent failed.");
            }
        }

        //kev.push_back(e_write);

        // logger_->info("finished register write");
    }

    channels_[fd] = ch;
    registeredEvents_[fd] = ev;
}

void KQueuePoller::removeChannel(Channel* ch) {
    const int fd = ch->fd();
    
    std::vector<struct kevent> kev;

    struct kevent e1;
    EV_SET(&e1, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kev.push_back(e1);

    struct kevent e2;
    EV_SET(&e2, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kev.push_back(e2);

    if (::kevent(kqueueFd_, kev.data(), kev.size(), nullptr, 0, nullptr) == -1) {
        logger_->warn("remove kqueue got error.");
    }

    channels_.erase(fd);
    registeredEvents_.erase(fd);
}

void KQueuePoller::poll(int timeoutMs, std::vector<Channel*>& activeChannels) {
    struct timespec ts;
    ts.tv_sec = timeoutMs / 1000;
    ts.tv_nsec = (timeoutMs % 1000) * 1000000;

    int n = ::kevent(kqueueFd_, nullptr, 0, events_.data(), static_cast<int>(events_.size()), &ts);
    if (n < 0) {
        logger_->error("kevent error.");
        //TODO Handle error properly, maybe throw an exception or log the error
    }

    for (int i = 0; i < n; ++i) {
        auto & e = events_[i];
        Channel* ch = static_cast<Channel*>(e.udata);

        if (!ch) {
            continue;
        }

        IOEvent re = IO_NONE;

        if (events_[i].filter == EVFILT_READ) {
            re |= IO_READ;
        }
        if (events_[i].filter == EVFILT_WRITE) {
            re |= IO_WRITE;
        }

        if (e.flags & EV_ERROR) {
            re |= IO_ERROR;
        }

        ch->setRevents(re);
        activeChannels.push_back(ch);
    }
}

} // namespace reactor
