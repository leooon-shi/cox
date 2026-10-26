#pragma once

#include <atomic>
#include <cstddef>

namespace flowcontrol {
// A concurrency limiter that bounds the number of in-flight units of work
// (e.g. concurrent requests being processed), as opposed to a rate limiter
// which bounds the rate of admission over time.
//
// Connection count is not a good proxy for load: many idle keep-alive
// connections are cheap, while a few connections each with heavy in-flight
// work can saturate CPU/memory. This limiter caps the concurrent work directly.
//
// Usage (RAII permit):
//   ConcurrencyLimiter limiter(100);
//   if (auto permit = limiter.tryAcquire()) {
//       // admitted: do the work; permit releases on scope exit
//   } else {
//       // over capacity: shed load (reject early / notify)
//   }
//
// Thread-safe (lock-free counter).
class ConcurrencyLimiter {
public:
    // RAII permit. Holds a slot while alive; releases it on destruction or
    // move. A default/empty permit (from a failed tryAcquire) is falsy.
    class Permit {
    public:
        Permit() : limiter_(nullptr) {}
        explicit Permit(ConcurrencyLimiter* l) : limiter_(l) {}

        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;

        Permit(Permit&& other) noexcept : limiter_(other.limiter_) {
            other.limiter_ = nullptr;
        }
        Permit& operator=(Permit&& other) noexcept {
            if (this != &other) {
                release();
                limiter_ = other.limiter_;
                other.limiter_ = nullptr;
            }
            return *this;
        }
        ~Permit() { release(); }

        // True if this permit represents a successfully acquired slot.
        explicit operator bool() const { return limiter_ != nullptr; }

        // Release early (before scope exit). Idempotent.
        void release() {
            if (limiter_) {
                limiter_->release();
                limiter_ = nullptr;
            }
        }

    private:
        ConcurrencyLimiter* limiter_;
    };

    // max_concurrency: maximum in-flight units. 0 means unlimited.
    explicit ConcurrencyLimiter(size_t max_concurrency)
        : max_(max_concurrency), in_flight_(0) {}

    // Try to acquire a slot without waiting. Returns a truthy Permit on success
    // (slot held until the Permit is destroyed/released), or an empty (falsy)
    // Permit when at capacity.
    Permit tryAcquire() {
        if (max_ == 0) {
            // Unlimited: still track in-flight for metrics, always admit.
            in_flight_.fetch_add(1, std::memory_order_relaxed);
            return Permit(this);
        }
        size_t cur = in_flight_.load(std::memory_order_relaxed);
        while (cur < max_) {
            if (in_flight_.compare_exchange_weak(cur, cur + 1,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {
                return Permit(this);
            }
            // cur reloaded by compare_exchange_weak on failure; loop retries.
        }
        return Permit();  // at capacity -> empty permit
    }

    size_t inFlight() const {
        return in_flight_.load(std::memory_order_relaxed);
    }
    size_t maxConcurrency() const { return max_; }
    bool isOverloaded() const {
        return max_ != 0 && in_flight_.load(std::memory_order_relaxed) >= max_;
    }

private:
    friend class Permit;
    void release() { in_flight_.fetch_sub(1, std::memory_order_release); }

    const size_t max_;
    std::atomic<size_t> in_flight_;
};
}  // namespace flowcontrol
