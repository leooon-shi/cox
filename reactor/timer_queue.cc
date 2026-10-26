#include "timer_queue.h"

#include "event_loop.h"
#include "io_channel.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace reactor {
namespace {
int createTimerfd() {
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("timerfd_create failed: " +
                                 std::string(std::strerror(errno)));
    }
    return fd;
}

// Time from now until `when`, clamped to a minimum of 1 microsecond so the
// timerfd always arms (a zero itimerspec would disarm it).
struct timespec howMuchTimeFromNow(logging::Timestamp when) {
    int64_t micros = static_cast<int64_t>(when.microSecondsSinceEpoch()) -
                     static_cast<int64_t>(
                         logging::Timestamp::now().microSecondsSinceEpoch());
    if (micros < 1) {
        micros = 1;
    }

    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(micros /
                                    logging::Timestamp::kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>(
        (micros % logging::Timestamp::kMicroSecondsPerSecond) * 1000);
    return ts;
}

void readTimerfd(int fd) {
    uint64_t howmany = 0;
    ssize_t n = ::read(fd, &howmany, sizeof(howmany));
    (void)n;  // count of expirations; not needed here
}

void resetTimerfd(int fd, logging::Timestamp expiration) {
    struct itimerspec new_value;
    struct itimerspec old_value;
    std::memset(&new_value, 0, sizeof(new_value));
    std::memset(&old_value, 0, sizeof(old_value));
    new_value.it_value = howMuchTimeFromNow(expiration);
    if (::timerfd_settime(fd, 0, &new_value, &old_value) < 0) {
        // Best-effort; a failure here means the timer won't fire.
    }
}

}  // namespace

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfd_channel_(std::make_unique<Channel>(timerfd_, loop)) {
    timerfd_channel_->setReadCallback([this] { handleRead(); });
    timerfd_channel_->enableRead();
}

TimerQueue::~TimerQueue() {
    timerfd_channel_->disableAll();
    loop_->removeChannel(timerfd_channel_.get());
    ::close(timerfd_);
    for (const auto& entry : timers_) {
        delete entry.second;
    }
}

TimerId TimerQueue::addTimer(TimerCallback cb, logging::Timestamp when,
                             uint64_t interval_ms) {
    Timer* timer = new Timer(std::move(cb), when, interval_ms);
    const bool earliest_changed = insert(timer);
    active_sequences_.insert(timer->sequence());
    if (earliest_changed) {
        resetTimerfd(timerfd_, timer->expiration());
    }

    return TimerId(timer, timer->sequence());
}

void TimerQueue::cancel(TimerId id) {
    if (id.timer_ == nullptr) {
        return;
    }
    auto it = timers_.find(Entry(id.timer_->expiration(), id.timer_));
    if (it != timers_.end()) {
        active_sequences_.erase(it->second->sequence());
        delete it->second;
        timers_.erase(it);
    } else if (calling_expired_callbacks_) {
        // The timer is currently being processed (a repeating timer cancelling
        // itself). Defer removal so reset() won't re-arm it.
        cancelling_.insert(id.sequence_);
    }
}

bool TimerQueue::insert(Timer* timer) {
    bool earliest_changed = false;
    logging::Timestamp when = timer->expiration();
    auto it = timers_.begin();
    if (it == timers_.end() || when < it->first) {
        earliest_changed = true;
    }
    timers_.insert(Entry(when, timer));
    return earliest_changed;
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(logging::Timestamp now) {
    std::vector<Entry> expired;
    // Sentinel that is strictly greater than any real Entry with time `now`.
    Entry sentinel(now, reinterpret_cast<Timer*>(UINTPTR_MAX));
    auto end = timers_.upper_bound(sentinel);
    for (auto it = timers_.begin(); it != end; ++it) {
        expired.push_back(*it);
    }

    timers_.erase(timers_.begin(), end);
    return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired,
                       logging::Timestamp now) {
    for (const auto& entry : expired) {
        Timer* timer = entry.second;
        const uint64_t seq = timer->sequence();
        if (timer->repeat() && cancelling_.find(seq) == cancelling_.end()) {
            timer->restart(now);
            insert(timer);
        } else {
            active_sequences_.erase(seq);
            delete timer;
        }
    }

    if (!timers_.empty()) {
        resetTimerfd(timerfd_, timers_.begin()->first);
    }
}

void TimerQueue::handleRead() {
    logging::Timestamp now = logging::Timestamp::now();
    readTimerfd(timerfd_);

    std::vector<Entry> expired = getExpired(now);

    calling_expired_callbacks_ = true;
    cancelling_.clear();
    for (const auto& entry : expired) {
        // Skip timers cancelled during this batch of callbacks.
        if (cancelling_.find(entry.second->sequence()) == cancelling_.end()) {
            entry.second->run();
        }
    }

    calling_expired_callbacks_ = false;

    reset(expired, now);
}
}  // namespace reactor