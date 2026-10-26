#pragma once

#include "timestamp.h"

#include <atomic>
#include <cstdint>
#include <functional>

namespace reactor
{
// A single scheduled timer. Owns its callback and knows when it should next
// fire. Repeating timers re-arm themselves relative to the moment they expire.

class Timer
{
public:
    using TimerCallback = std::function<void()>;

    Timer(TimerCallback cb, logging::Timestamp when, uint64_t interval_ms)
    : callback_(std::move(cb)),
      expiration_(when),
      interval_ms_(interval_ms),
      repeat_(interval_ms > 0),
      sequence_(++s_num_created_)
    {
    }
    
    void run() const
    {
        if (callback_) callback_();
    }

    logging::Timestamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    uint64_t sequence() const { return sequence_; }

    // Advance the expiration by one interval from `now`. Only valid for
    // repeating timers; one-shot timers become invalid (never re-armed).
    void restart(logging::Timestamp now)
    {
        if (repeat_)
        {
            expiration_ = logging::addMilliseconds(now, interval_ms_);
        }
        else
        {
            expiration_ = logging::Timestamp::invalid();
        }
    }

    static uint64_t numCreated() { return s_num_created_.load(); }

private:
    const TimerCallback callback_;
    logging::Timestamp expiration_;
    const uint64_t interval_ms_;
    const bool repeat_;
    const uint64_t sequence_;

    static std::atomic<uint64_t> s_num_created_;
};

// Opaque handle used to cancel a previously scheduled timer.
class TimerId {
public:
    TimerId() : timer_(nullptr), sequence_(0) {}
    TimerId(Timer* timer, uint64_t seq) : timer_(timer), sequence_(seq) {}

    friend class TimerQueue;

private:
    Timer* timer_;
    uint64_t sequence_;
};
} // namespace reactor

