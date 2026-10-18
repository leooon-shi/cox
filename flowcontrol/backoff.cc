#include "backoff.h"

#include <algorithm>

namespace flowcontrol
{
namespace
{
// Seed the PRNG from the clock so different processes/instances don't jitter
// identically.
uint64_t seedFromClock()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t s = static_cast<uint64_t>(now);
    return s ? s : 0x9e3779b97f4a7c15ULL;
}
} // namespace

Backoff::Backoff(Duration initial, Duration max, double jitter)
: initial_(initial.count() > 0 ? initial : Duration(1)),
  max_(max >= initial ? max : initial),
  jitter_(jitter < 0 ? 0 : jitter),
  rng_state_(seedFromClock())
  {
  }

Backoff::Duration Backoff::applyJitter(Duration d)
{
    if(jitter_ <= 0)
    {
        return d;
    }
    //xorshift64->uniform double n [0,1).
    rng_state_ ^= rng_state_ << 13;
    rng_state_ ^= rng_state_ >> 7;
    rng_state_ ^= rng_state_ << 17;
    
    double r = static_cast<double>(rng_state_ >> 11) / static_cast<double>(1ULL << 53);
    double factor = 1.0 + jitter_ * r; // in [1, jitter)
    return Duration(static_cast<Duration::rep>(d.count() * factor));
}

void Backoff::next(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = Clock::now();
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        entries_[key] = Entry{initial_, now};
        return;
    }
    // Double, capped at max_.
    Duration doubled = it->second.duration * 2;
    it->second.duration = std::min(doubled, max_);
    it->second.last_update = now;
}

Backoff::Duration Backoff::get(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    Duration base = (it == entries_.end()) ? initial_ : it->second.duration;
    return applyJitter(base);
}

Backoff::Duration Backoff::nextDelay(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = Clock::now();
    auto it = entries_.find(key);
    Duration base;
    if (it == entries_.end())
    {
        entries_[key] = Entry{initial_, now};
        base = initial_;
    }
    else
    {
        Duration doubled = it->second.duration * 2;
        it->second.duration = std::min(doubled, max_);
        it->second.last_update = now;
        base = it->second.duration;
    }
    return applyJitter(base);
}

void Backoff::reset(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(key);
}

void Backoff::gc(Duration max_age)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = Clock::now();
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (now - it->second.last_update > max_age)
        {
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

Backoff::Duration Backoff::baseDelay(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    return (it == entries_.end()) ? initial_ : it->second.duration;
}

} // namespace flowcontrol