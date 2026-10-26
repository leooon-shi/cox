#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace flowcontrol
{
// Per-key exponential backoff with jitter, modeled after client-go's 
// flowcontrol.Backoff. Each key (e.g. a server address, a client id) has its
// own backoff state that grows exponentially on repeated failures and resets
// after a period of success/inactivity.
//
// Jitter is applied to the returned delay to avoid a thundering herd when many
// keys back off in lockstep (e.g. mass reconnect after a server restart).
//
// Typical use:
//   Backoff bo(1s, 30s);            // initial 1s, cap 30s
//   ... on failure:
//   bo.next(key);                   // record failure, advance backoff
//   auto delay = bo.get(key);       // jittered delay to wait before retry
//   ... on success:
//   bo.reset(key);                  // clear backoff for the key
//
// All methods are thread-safe.
class Backoff
{
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::milliseconds;

    // initial: base backoff duration.
    // max:     cap on the backoff duration.
    // jitter:  fractional jitter in [0,1). The returned delay is multiplied by
    //          a random factor in [1, 1+jitter). Default 0.5 (client-go-like).
    Backoff(Duration initial, Duration max, double jitter = 0.5);

    // Advance the backoff for `key` (call on failure): doubles the current
    // duration up to the cap. Creates the entry at `initial` if new.
    void next(const std::string& key);

    // Return the current jittered backoff delay for `key` without advancing it.
    // Returns a jittered `initial` for an unknown key.
    Duration get(const std::string& key);

    // Advance and return the jittered delay in one call (failure + get).
    Duration nextDelay(const std::string& key);

    // Clear backoff state for `key` (call on success).
    void reset(const std::string& key);

    // Remove entries not updated within `max_age` (housekeeping to bound the
    // map size for churny key spaces).
    void gc(Duration max_age);

    // Base (un-jittered) current backoff for `key`, for tests/metrics.
    Duration baseDelay(const std::string& key);

private:
    struct Entry {
        Duration duration;
        Clock::time_point last_update;
    };

    Duration applyJitter(Duration d);

    const Duration initial_;
    const Duration max_;
    const double jitter_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    uint64_t rng_state_;  // xorshift, guarded by mutex_
};
}   // namepace flowcontrol
