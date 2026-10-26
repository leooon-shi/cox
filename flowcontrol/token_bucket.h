#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace flowcontrol
{
// A thread-safe token-bucket rate limiter, modeled after client-go's
// flowcontrol.RateLimiter (which wraps golang.org/x/time/rate).
//
// Tokens accrue continuously at `qps` per second up to a maximum of `burst`
// tokens. Each admitted event consumes one token. This smooths a bursty
// arrival pattern to an average rate while permitting short bursts up to the
// bucket capacity.
//
// Refill is lazy (computed on access from elapsed time), so there is no
// background timer. All methods are safe to call from multiple threads.
class TokenBucket
{
public:
    using Clock = std::chrono::steady_clock;

    // qps: sustained tokens added per second (>0).
    // burst: maximum tokens the bucket can hold (>= 1). The bucket starts full.
    TokenBucket(double qps, double burst);

    // Try to consume one token without waiting. Returns true if a token was
    // available (event admitted), false otherwise (should be rejected/deferred).
    bool tryAcquire();

    // Try to consume n tokens without waiting.
    bool tryAcquireN(double n);

    // Current available tokens (after lazy refill). Primarily for tests/metrics
    double availableTokens();

    double qps() const { return qps_; }
    double burst() const { return burst_; }
private:
    void refillLocked(Clock::time_point now);       // caller holds mutex_

    const double qps_;
    const double burst_;
    double tokens_;
    Clock::time_point last_refill_;
    std::mutex mutex_;
};

} // namespace flowcontrol