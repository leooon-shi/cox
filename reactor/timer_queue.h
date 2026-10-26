#pragma once

#include "timer.h"
#include "timestamp.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace reactor {

class EventLoop;
class Channel;

// TimerQueue schedules callbacks to run at (or after) a given time, using a
// single Linux timerfd registered with the owning EventLoop. All mutating
// operations must happen in the loop thread; EventLoop is responsible for
// marshalling cross-thread calls via runInLoop before touching the queue.
class TimerQueue {
public:
    using TimerCallback = std::function<void()>;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    // Schedule `cb` to run at absolute time `when`. If interval_ms > 0 the
    // timer repeats every interval_ms milliseconds. Must be called in the
    // loop thread.
    TimerId addTimer(TimerCallback cb, logging::Timestamp when,
                     uint64_t interval_ms);

    // Cancel a previously scheduled timer. Must be called in the loop thread.
    void cancel(TimerId id);

private:
    using Entry = std::pair<logging::Timestamp, Timer*>;
    using TimerSet = std::set<Entry>;

    // Called when the timerfd becomes readable.
    void handleRead();

    // Return all timers that have expired by `now`, removing them from the set.
    std::vector<Entry> getExpired(logging::Timestamp now);

    // Re-arm repeating timers among `expired` and reset the timerfd.
    void reset(const std::vector<Entry>& expired, logging::Timestamp now);

    // Insert a timer into the ordered set; returns true if it becomes the
    // earliest timer (meaning the timerfd needs re-arming).
    bool insert(Timer* timer);

    EventLoop* loop_;
    const int timerfd_;
    std::unique_ptr<Channel> timerfd_channel_;

    // Timers ordered by expiration time.
    TimerSet timers_;
    // Sequence numbers of timers currently active (for O(log n) cancel).
    std::set<uint64_t> active_sequences_;
    // Sequence numbers cancelled while callbacks are running (self-cancel).
    std::set<uint64_t> cancelling_;
    bool calling_expired_callbacks_{false};
};

}  // namespace reactor
