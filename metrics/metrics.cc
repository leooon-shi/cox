#include "metrics.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace metrics {
namespace {
// Reinterpret a double as its IEEE-754 bit pattern and back, so a double sum
// can live in a std::atomic<uint64_t> and be updated with a CAS loop.
uint64_t toBits(double d) noexcept {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

double fromBits(uint64_t bits) noexcept {
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}
}  // namespace

Histogram::Histogram(std::vector<double> upper_bounds)
    : bounds_(std::move(upper_bounds)),
      counts_(bounds_.size() + 1) {  // +1 for the implicit +Inf bucket
    if (bounds_.empty()) {
        throw std::invalid_argument("Histogram requires >= 1 upper bound");
    }
    for (size_t i = 1; i < bounds_.size(); ++i) {
        if (!(bounds_[i] > bounds_[i - 1])) {
            throw std::invalid_argument(
                "Histogram bounds must be strictly ascending");
        }
    }
    // std::atomic is not copyable, so counts_ is value-initialized to 0 by the
    // vector<atomic> constructor above.
}

void Histogram::observe(double v) noexcept {
    // First bucket whose upper bound is >= v; if none, the +Inf overflow bucket
    // (index bounds_.size()).
    size_t idx = static_cast<size_t>(
        std::lower_bound(bounds_.begin(), bounds_.end(), v) - bounds_.begin());
    counts_[idx].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);

    // Atomically add v to the double sum via a CAS loop on its bit pattern.
    uint64_t old_bits = sum_bits_.load(std::memory_order_relaxed);
    uint64_t new_bits;
    do {
        new_bits = toBits(fromBits(old_bits) + v);
    } while (!sum_bits_.compare_exchange_weak(old_bits, new_bits,
                                              std::memory_order_relaxed));
}

Histogram::Snapshot Histogram::snapshot() const {
    Snapshot s;
    s.bounds = bounds_;
    s.counts.resize(counts_.size());
    // Emit CUMULATIVE counts (Prometheus le-bucket semantics): each bucket
    // includes all observations <= its upper bound.
    uint64_t cumulative = 0;
    for (size_t i = 0; i < counts_.size(); ++i) {
        cumulative += counts_[i].load(std::memory_order_relaxed);
        s.counts[i] = cumulative;
    }
    s.count = count_.load(std::memory_order_relaxed);
    s.sum = fromBits(sum_bits_.load(std::memory_order_relaxed));
    return s;
}

std::vector<double> Histogram::defaultLatencyBuckets() {
    return {0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05,
            0.1,    0.25,  0.5,    1.0,   2.5,  5.0,   10.0};
}

}  // namespace metrics
