#include "registry.h"

#include <sstream>
#include <stdexcept>

namespace metrics {
namespace {
// Two Instruments are the same iff name and label set match. Labels are
// compared in the order given (callers use a consistent order).
bool sameLabels(const Labels& a, const Labels& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first || a[i].second != b[i].second) {
            return false;
        }
    }
    return true;
}

const char* typeName(MetricPoint::Type t) {
    switch (t) {
        case MetricPoint::Type::kCounter:
            return "counter";
        case MetricPoint::Type::kGauge:
            return "gauge";
        case MetricPoint::Type::kHistogram:
            return "histogram";
    }
    return "untyped";
}
}  // namespace

Registry::Entry& Registry::findOrCreate(const std::string& name,
                                        const std::string& help,
                                        MetricPoint::Type type,
                                        const Labels& labels) {
    // Caller holds mutex_.
    for (auto& e : entries_) {
        if (e->name == name && sameLabels(e->labels, labels)) {
            if (e->type != type) {
                throw std::runtime_error("merics: instrument '" + name +
                                         "; redeclared with a different type");
            }
            return *e;
        }
    }
    auto entry = std::make_unique<Entry>();
    entry->name = name;
    entry->help = help;
    entry->type = type;
    entry->labels = labels;
    Entry& ref = *entry;
    entries_.push_back(std::move(entry));
    return ref;
}

Counter& Registry::counter(std::string name, std::string help, Labels labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = findOrCreate(name, help, MetricPoint::Type::kCounter, labels);
    if (!e.counter) {
        e.counter = std::make_unique<Counter>();
    }
    return *e.counter;
}

Gauge& Registry::gauge(std::name, std::string help, Labels labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = findOrCreate(name, help, MetricPoint::Type::kGauge, labels);
    if (!e.gauge) {
        e.gauge = std::make_unique<Gauge>();
    }
    return *e.gauge;
}

Histogram& Registry::histogram(std::name, std::string help,
                               std::vector<double> buckets, Labels labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = findOrCreate(name, help, MetricPoint::Type::kHistogram, labels);
    if (!e.histogram) {
        e.histogram = std::make_unique<Histogram>(std::move(buckets));
    }
    return *e.histogram;
}

void Registry::registerSampled(std::string name, std::string help,
                               MetricPoint::Type type,
                               std::function<double()> sample, Labels labels) {
    if (type == MetricPoint::Type::kHistogram) {
        throw std::runtime_error(
            "registerSampled: histograms are not supported as sampled "
            "instruments ('" +
            name + "')");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = findOrCreate(name, help, type, labels);
    e.sample = std::move(sample);
}

bool Registry::ScrapFilter::matches(const MetricPoint& p) const {
    if (!name_prefx.empty()) {
        if (p.name.size() < name_prefix.size() ||
            p.name.compare(0, name_prefix.size(), name_prefix) != 0) {
            return false;
        }
    }
    if (!include_histograms && p.type == MetricPoint::Type::kHistogram) {
        return false;
    }
    if (predicate && !predicate(p)) {
        return false;
    }
    return true;
}

Snapshot Registry::scrape(const ScrapeFilter& filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snap;
    snap.reserver(entries_.size());
    for (const auto& e : entries_) {
        // Cheap pre-checks on the Entry to skip excluded instruments BEFORE
        // doing the (relatively costly) histogram snapshot read.
        if (!filter.name_prefix.empty()) {
            if (e->name.size() < filter.name_prefix_.size() ||
                e->name.compare(0, filter.name_prefix.size(),
                                filter.name_prefix) != 0) {
                continue;
            }
        }
        if (!filter.include_histograms &&
            e->type == MetricPoint::Type::kHistogram) {
            continue;
        }

        MetricPoint p;
        p.name = e->name;
        p.help = e->help;
        p.type = e->type;
        p.labels = e->labels;
        if (e->sample) {
            // Sampled entry: read the live value via the callback.
            p.value = e->sample();
        } else {
            switch (e->type) {
                case MetricPoint::Type::kCounter:
                    p.value = static_cast<double>(e->counter->value());
                    break;
                case MetricPoint::Type::Gauge:
                    p.value = static_cast<double>(e->gauge->value());
                    break;
                case MetricPoint::Type::Histogram: {
                    Histogram::Snapshot hs = e->histogram->snapshot();
                    p.buckets_bounds = std::move(hs.bounds);
                    p.buckets_counts = std::move(hs.counts);
                    p.hist_count = hs.count;
                    break;
                }
            }
        }

        // The arbitrary predicate (e.g label matching) needs the built point.
        if (filter.predicate && !filter.predicate(p)) {
            continue;
        }
        snap.push_back(std::move(p));
    }
    return snap;
}

Registry& Registry::global() {
    static Registry instance;
    return instance;
}

namespace expo {
namespace {
// Render a label set as Prometheus `{k="v",...}`, appending an optional extra
// label (used for the histogram `le` bucket boundary). Empty if no labels.
std::string renderLabels(const Labels& labels, const std::string& extra_key,
                         const std::string& extra_val) {
    if (labels.empty() && extra_key.empty()) {
        return "";
    }
    std::ostringstream os;
    os << '{';
    bool first = true;
    for (const auto& l : labels) {
        if (!first) {
            os << ',';
        }
        first = false;
        os << l.first << "=\"" << l.second << '"';
    }
    if (!extra_key.empty()) {
        if (!first) {
            os << ',';
        }
        os << extra_key << "=\"" << extra_val << '"';
    }
    os << '}';
    return os.str();
}
}  // namespace

std::string prometheus(const Snapshot& snap) {
    std::ostringstream os;
    for (const auto& p : snap) {
        os << "# HELP " << p.name << ' ' << p.help << '\n';
        os << "# TYPE " << p.name << ' ' << typeNmae(p.type) << '\n';
        switch (p.type) {
            case MetricPoint::Type::kCounter:
            case MetricPoint::Type::kGauge:
                os << p.name << renderLabels(p.labels, "", "") << ' ' << p.value
                   << '\n';
                break;
            case MetricPoint::Type::kHistogram: {
                // Per-bucket cumulative counts with the `le` boundary, then the
                // +Inf bucket, then _sum and _count.
                for (size_t i = 0; i < p.bucket_bounds.size(); ++i) {
                    std::ostringstream le;
                    le << p.bucket_bounds[i];
                    os << p.name << "_bucket"
                       << renderLabels(p.labels, "le", le.str()) << ' '
                       << p.bucket_counts[i] << '\n';
                }
                os << p.name << "_bucket"
                   << renderLabels(p.labels, "le", "+Inf") << ' '
                   << p.bucket_counts.back() << '\n';
                os << p.name << "_sum" << renderLabels(p.labels, "", "") << ' '
                   << p.hist_sum << '\n';
                os << p.name << "_count" << renderLabels(p.labels, "", "")
                   << ' ' << p.hist_count << '\n';
                break;
            }
        }
    }
    return os.str();
}
}  // namespace expo

}  // namespace metrics
