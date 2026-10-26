#include "token_bucket.h"

#include <algorithm>

namespace flowcontrol {
TokenBucket::TokenBucket(double qps, double burst)
    : qps_(qps > 0 ? qps : 0),
      burst_(burst >= 1 ? burst : 1),
      tokens_(burst_),  // start full
      last_refill_(Clock::now()) {}

void TokenBucket::refillLocked(Clock::time_point now) {
    if (now <= last_refill_) {
        return;
    }
    const double elapsed_s =
        std::chrono::duration<double>(now - last_refill_).count();
    last_refill_ = now;
    tokens_ = std::min(burst_, tokens_ + elapsed_s * qps_);
}

bool TokenBucket::tryAcquire() {
    return tryAcquireN(1.0);
}

bool TokenBucket::tryAcquireN(double n) {
    if (n <= 0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    refillLocked(Clock::now());
    if (tokens_ >= n) {
        tokens_ -= n;
        return true;
    }
    return false;
}

double TokenBucket::availableTokens() {
    std::lock_guard<std::mutex> lock(mutex_);
    refillLocked(Clock::now());
    return tokens_;
}

}  // namespace flowcontrol
