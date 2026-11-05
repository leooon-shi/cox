#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace metrics {
// Metric instrument primitives: Counter, Gauge, Histogram.
//
// Design (see docs/insights/features/metrics-design.md):
//   * Hot-path operations (inc/dec/observe) are lock-free relaxed atomics --
//     correct for metrics (atomicity, not ordering) and cheap. No instrument
//     takes a lock to record a value.
//   * Reads (value/snapshot) run off the hot path, at scrape time.
//   * Instruments are non-copyable, non-movable: the Registry owns them and
//     hands out stable references, so callers cache the reference and never
//     look up by name on the hot path.

// Counter: a monotonically increasing value (resets only on process restart).
// Rates are computed by the scraper (e.g. Prometheus rate()), not here.
class Counter {
public:
    Counter() = default;
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;

    void inc(uint64_t n = 1) noexcept {
        value_.fetch_add(n, std::memory_order_relaxed);
    }
    uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> value_{0};
};

// Gauge: a value that can go up and down. Use inc/dec for a tracked quantity
// (e.g. live connections) and set() for a sampled one (e.g. queue depth read
// at scrape time).
class Gauge {
public:
    Gauge() = default;
    Gauge(const Gauge&) = delete;
    Gauge& operator=(const Gauge&) = delete;

    void inc(int64_t n = 1) noexcept {
        value_.fetch_add(n, std::memory_order_relaxed);
    }
    void dec(int64_t n = 1) noexcept {
        value_.fetch_sub(n, std::memory_order_relaxed);
    }
    void set(int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }
    int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> value_{0};
};

// Histogram: a distribution over fixed, explicit upper-bound buckets
// (Prometheus "classic" histogram). Bucket counters are cheap atomics and
// aggregate across instances; quantiles are computed by the scraper. Local
// quantile summaries are deliberately NOT provided (they do not aggregate).
class Histogram {
public:
    // upper_bounds: ascending, finite, and WITHOUT the implicit +Inf bucket
    // (which is tracked internally as the overflow bucket). Throws
    // std::invalid_argument if empty or not strictly ascending.
    explicit Histogram(std::vector<double> upper_bounds);
    Histogram(const Histogram&) = delete;
    Histogram& operator=(const Histogram&) = delete;

    // Record one observation. Hot path: find the first bucket whose upper
    // bound >= v and increment it, plus count and sum.
    void observe(double v) noexcept;

    // Cumulative, Prometheus-style view of the distribution.
    struct Snapshot {
        std::vector<double> bounds;  // the configured upper bounds (no +Inf)
        std::vector<uint64_t>
            counts;         // size == bounds.size()+1; cumulative,
                            // last element is the +Inf bucket == total
        uint64_t count{0};  // total observations
        double sum{0.0};    // sum of observed values
    };
    Snapshot snapshot() const;

    // A reasonable default bucket set for latency in SECONDS.
    static std::vector<double> defaultLatencyBuckets();

private:
    std::vector<double> bounds_;  // ascending upper bounds (no +inf)
    std::vector<std::atomic<uint64_t>> counts_;  // per-bucket, size bounds_+1
    std::atomic<uint64_t> count_{0};
    // sum as atomic bits of a double (relaxed CAS loop in observe()).
    std::atomic<uint64_t> sum_bits_{0};
};

}  // namespace metrics
