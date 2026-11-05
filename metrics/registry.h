#pragma once

#include "metrics.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace metrics {
// A label is key=value pair; a metric's identity is its name plus an ordered
// label set. For v1 (see design §4.4) callers pre-declare the label values and
// hold the returned instrument reference, so there is no per-precord map
// lookup.
using Label = std::pair<std::string, std::string>;
using Labels = std::vector<Label>;

// Neutral, format-independent view of one instrument at a point in time. Every
// exporter (Prometheus, OpenTelemetry, a test matcher) is a pure function of a
// vector of these -- see design §5. Adding an exporter never touches the
// instruments.
struct MetricPoint {
    enum class Type { kCounter, kGauge, kHistogram };
    std::string name;
    std::string help;
    Type type;
    Labels labels;

    // Counter / Gauge value.
    double value{0.0};

    // Histogram (empty for non-histograms). Cumulative, Prometheus-style.
    std::vector<double> bucket_bounds;    // asceding, excludes +Inf
    std::vector<uint64_t> bucket_counts;  // size == bounds+1 (last == +Inf)
    uint64_t hist_count{0};
    double hist_sum{0.0};
};
using Snapshot = std::vector<MetricPoint>;

// Registry owns the instruments and is the single read seam (scrape()). It is
// safe to create instruments from multiple threads; creation takes a mutex and
// happens at startup, not on the hot path. The hot path uses the returned
// instrument reference directly and never touches the registry.
class Registry {
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Get-or-create an instrument. Returns a stable reference (the registry
    // owns the object). Calling again with the same (name, labels) returns the
    // SAME instrument; the type must match or std::runtime_error is thrown.
    Counter& counter(std::string name, std::string help, Labels labels = {});
    Gauge& gauge(std::string name, std::string help, Labels label = {});
    Histogram& histogram(std::string name, std::string help,
                         std::vector<double> buckets, Labels labels = {});

    // Register a SAMPLED instrument whose value is read from `sample()` at
    // scrape time, rather than stored in an owned atomic. Use this to surface a
    // value that lives elsewhere and is updated on a hot path with no metrics
    // dependency -- e.g. EventLoop::iterations() or a thread-pool queue depth.
    // `type` is kCounter (monotonic source) or kGuage (up/down sources);
    // kHistogram is not supported here. `sample` is invoked during scrape(),
    // so it must be cheap and thread-safe (reading a relaxed atomic is both).
    void registerSampled(std::string name, std::string help,
                         MetricPoint::Type type, std::function<double()> sample,
                         Labels labels = {});

    // Scrape-time filter: selects WHICH instruments a scrape emits. It only
    // affects the scraper's work (iteration + later serialization) -- the hot
    // path (inc/observe) is untoched. Use it to run a cheap, frequent scrape
    // over subset (e.g. counters only, or a name prefix) alongside an
    // occasional full scrape.
    //
    // A default-constructed filter selects everything. Rules are ANDed: an
    // instrument is included only if it passes every set rule.
    struct ScrapeFilter {
        // Keep only instruments whose name starts with this prefix. Empty =
        // no prefix constraint.
        std::string name_prefix;
        // If false, histograms are skipped. They are the most expensive to
        // serialize, so a frequent lightweight scrape can exclude them.
        bool include_histograms = true;
        // Optional arbitrary predicate for anything the fields above don't
        // cover (e.g. label matching). Null = no extra constraint.
        std::function<bool(const MetricPoint&)> predicate;

        // True if `p` passes all set rules.
        bool matches(const MetricPoint& p) const;
    };

    // Point-in-time, format-neutral snapshot of the instruments selected by
    // `filter`. Thread-safe; takes the registry mutex only to iterate, never
    // on the hot path.
    Snapshot scrape(const ScrapeFilter& filter) const;
    // Convenience overload: snapshot of ALL instruments (no filters).
    Snapshot scrape() const { return scrape(ScrapeFilter{}); }

    // Process-wide default registry (mirrors the Logger's default instance)
    static Registry& global();

private:
    struct Entry {
        std::string name;
        std::string help;
        MetricPoint::Type type;
        Labels labels;
        // Exactly one of these describes the value source:
        //  - an owned intrument (counter/gauge/histogram), OR
        //  - `sample` (a scrape-time callback) for a sampled counter/gauge.
        std::unique_ptr<Counter> counter;
        std::unique_ptr<Gauge> gauge;
        std::unique_ptr<Histogram> histogram;
        std::function<double()> sample;  // non-null for a sampled entry
    };

    Entry& findOrCreate(const std::string& name, const std::string& help,
                        MetricPoint::Type type, const Labels& labels);
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;  // stable addresses
};

// Exposition serializers over a Snapshot. Pure functions: no I/O, no registry
// coupling.
namespace expo {
// Prometheus text exposition format (version 0.0.4).
std::string prometheus(const Snapshot& snap);
}  // namespace expo

}  // namespace metrics
