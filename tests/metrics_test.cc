// Metrics module tests: instrument arithmetic, histogram bucketing, the
// format-neutral Snapshot, Prometheus exposition, and concurrent correctness.

#include "metrics.h"
#include "registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace metrics;

// ---- Counter / Gauge ----

TEST(MetricsTest, CounterIncrements) {
    Counter c;
    EXPECT_EQ(c.value(), 0u);
    c.inc();
    c.inc(5);
    EXPECT_EQ(c.value(), 6u);
}

TEST(MetricsTest, GaugeUpDownSet) {
    Gauge g;
    g.inc(10);
    g.dec(3);
    EXPECT_EQ(g.value(), 7);
    g.set(42);
    EXPECT_EQ(g.value(), 42);
    g.dec();
    EXPECT_EQ(g.value(), 41);
}

// ---- Histogram ----

TEST(MetricsTest, HistogramBucketsAndSum) {
    Histogram h({1.0, 5.0, 10.0});  // +Inf bucket is implicit
    h.observe(0.5);                 // -> bucket le=1
    h.observe(3.0);                 // -> bucket le=5
    h.observe(7.0);                 // -> bucket le=10
    h.observe(100.0);               // -> +Inf bucket

    auto s = h.snapshot();
    ASSERT_EQ(s.bounds.size(), 3u);
    ASSERT_EQ(s.counts.size(), 4u);  // bounds + 1
    // Cumulative (Prometheus le semantics).
    EXPECT_EQ(s.counts[0], 1u);  // <= 1
    EXPECT_EQ(s.counts[1], 2u);  // <= 5
    EXPECT_EQ(s.counts[2], 3u);  // <= 10
    EXPECT_EQ(s.counts[3], 4u);  // +Inf (total)
    EXPECT_EQ(s.count, 4u);
    EXPECT_DOUBLE_EQ(s.sum, 110.5);
}

TEST(MetricsTest, HistogramBoundaryGoesIntoItsOwnBucket) {
    Histogram h({1.0, 2.0});
    h.observe(1.0);  // exactly the boundary -> le=1 (lower_bound is inclusive)
    auto s = h.snapshot();
    EXPECT_EQ(s.counts[0], 1u);  // <= 1 includes exactly 1.0
    EXPECT_EQ(s.counts[1], 1u);  // <= 2 cumulative
    EXPECT_EQ(s.counts[2], 1u);  // +Inf
}

TEST(MetricsTest, HistogramRejectsBadBounds) {
    EXPECT_THROW(Histogram({}), std::invalid_argument);
    EXPECT_THROW(Histogram({5.0, 1.0}), std::invalid_argument);  // descending
    EXPECT_THROW(Histogram({1.0, 1.0}), std::invalid_argument);  // not strict
}

// ---- Registry & Snapshot ----

TEST(MetricsTest, RegistryGetOrCreateReturnsSameInstrument) {
    Registry reg;
    Counter& a = reg.counter("reqs", "requests");
    Counter& b = reg.counter("reqs", "requests");
    EXPECT_EQ(&a, &b);  // same identity -> same object
    a.inc(3);
    EXPECT_EQ(b.value(), 3u);
}

TEST(MetricsTest, RegistryTypeMismatchThrows) {
    Registry reg;
    reg.counter("x", "h");
    EXPECT_THROW(reg.gauge("x", "h"), std::runtime_error);
}

TEST(MetricsTest, LabelsDistinguishInstruments) {
    Registry reg;
    Counter& ok =
        reg.counter("http_requests_total", "by status", {{"status", "2xx"}});
    Counter& err =
        reg.counter("http_requests_total", "by status", {{"status", "5xx"}});
    EXPECT_NE(&ok, &err);
    ok.inc(10);
    err.inc(2);
    EXPECT_EQ(ok.value(), 10u);
    EXPECT_EQ(err.value(), 2u);
}

TEST(MetricsTest, ScrapeProducesNeutralSnapshot) {
    Registry reg;
    reg.counter("c", "a counter").inc(4);
    reg.gauge("g", "a gauge").set(9);
    reg.histogram("h", "a histogram", {1.0, 2.0}).observe(1.5);

    Snapshot snap = reg.scrape();
    ASSERT_EQ(snap.size(), 3u);

    // Snapshot carries type + values independent of any wire format.
    bool saw_counter = false, saw_gauge = false, saw_hist = false;
    for (const auto& p : snap) {
        if (p.name == "c") {
            saw_counter = true;
            EXPECT_EQ(p.type, MetricPoint::Type::kCounter);
            EXPECT_DOUBLE_EQ(p.value, 4.0);
        } else if (p.name == "g") {
            saw_gauge = true;
            EXPECT_EQ(p.type, MetricPoint::Type::kGauge);
            EXPECT_DOUBLE_EQ(p.value, 9.0);
        } else if (p.name == "h") {
            saw_hist = true;
            EXPECT_EQ(p.type, MetricPoint::Type::kHistogram);
            EXPECT_EQ(p.hist_count, 1u);
            EXPECT_DOUBLE_EQ(p.hist_sum, 1.5);
        }
    }
    EXPECT_TRUE(saw_counter && saw_gauge && saw_hist);
}

// ---- Prometheus exposition ----

TEST(MetricsTest, PrometheusFormat) {
    Registry reg;
    reg.counter("cox_connections_accepted_total", "accepted conns").inc(7);
    std::string out = expo::prometheus(reg.scrape());

    EXPECT_NE(out.find("# TYPE cox_connections_accepted_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cox_connections_accepted_total 7"), std::string::npos);
}

TEST(MetricsTest, PrometheusHistogramFormat) {
    Registry reg;
    auto& h = reg.histogram("lat", "latency", {0.1, 0.5});
    h.observe(0.05);  // le=0.1
    h.observe(0.3);   // le=0.5
    std::string out = expo::prometheus(reg.scrape());

    EXPECT_NE(out.find("# TYPE lat histogram"), std::string::npos);
    EXPECT_NE(out.find("lat_bucket{le=\"0.1\"} 1"), std::string::npos);
    EXPECT_NE(out.find("lat_bucket{le=\"0.5\"} 2"),
              std::string::npos);  // cumulative
    EXPECT_NE(out.find("lat_bucket{le=\"+Inf\"} 2"), std::string::npos);
    EXPECT_NE(out.find("lat_count 2"), std::string::npos);
}

TEST(MetricsTest, PrometheusLabelsRendered) {
    Registry reg;
    reg.counter("reqs", "by status", {{"status", "2xx"}}).inc(5);
    std::string out = expo::prometheus(reg.scrape());
    EXPECT_NE(out.find("reqs{status=\"2xx\"} 5"), std::string::npos);
}

// ---- Scrape-time filter ----

TEST(MetricsTest, ScrapeFilterByNamePrefix) {
    Registry reg;
    reg.counter("cox_http_requests_total", "h").inc(1);
    reg.counter("cox_tcp_bytes_total", "h").inc(2);
    reg.counter("app_widgets_total", "h").inc(3);

    Registry::ScrapeFilter f;
    f.name_prefix = "cox_http_";
    Snapshot snap = reg.scrape(f);

    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].name, "cox_http_requests_total");
}

TEST(MetricsTest, ScrapeFilterExcludesHistograms) {
    Registry reg;
    reg.counter("c", "h").inc(1);
    reg.gauge("g", "h").set(2);
    reg.histogram("hist", "h", {1.0, 2.0}).observe(0.5);

    Registry::ScrapeFilter f;
    f.include_histograms = false;
    Snapshot snap = reg.scrape(f);

    ASSERT_EQ(snap.size(), 2u);  // counter + gauge, no histogram
    for (const auto& p : snap) {
        EXPECT_NE(p.type, MetricPoint::Type::kHistogram);
    }
}

TEST(MetricsTest, ScrapeFilterPredicate) {
    Registry reg;
    reg.counter("reqs", "h", {{"status", "2xx"}}).inc(1);
    reg.counter("reqs", "h", {{"status", "5xx"}}).inc(1);

    Registry::ScrapeFilter f;
    f.predicate = [](const MetricPoint& p) {
        return !p.labels.empty() && p.labels[0].second == "5xx";
    };
    Snapshot snap = reg.scrape(f);

    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].labels[0].second, "5xx");
}

TEST(MetricsTest, ScrapeFilterRulesAreAnded) {
    Registry reg;
    reg.counter("cox_a_total", "h").inc(1);
    reg.histogram("cox_a_hist", "h", {1.0}).observe(0.5);  // matches prefix
    reg.counter("other_total", "h").inc(1);

    Registry::ScrapeFilter f;
    f.name_prefix = "cox_a";
    f.include_histograms = false;  // AND: prefix matches but histogram excluded
    Snapshot snap = reg.scrape(f);

    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].name, "cox_a_total");
}

TEST(MetricsTest, DefaultScrapeSelectsEverything) {
    Registry reg;
    reg.counter("c", "h");
    reg.histogram("h", "h", {1.0});
    EXPECT_EQ(reg.scrape().size(), 2u);  // no filter == all
}

// ---- Sampled instruments ----

TEST(MetricsTest, SampledInstrumentReadsLiveValueAtScrape) {
    Registry reg;
    uint64_t live = 0;
    reg.registerSampled("loop_iters", "iterations", MetricPoint::Type::kCounter,
                        [&live] { return static_cast<double>(live); });

    live = 5;
    EXPECT_EQ(reg.scrape()[0].value, 5.0);  // read at scrape time
    live = 99;
    EXPECT_EQ(reg.scrape()[0].value, 99.0);  // re-read, reflects new value
}

TEST(MetricsTest, SampledInstrumentRejectsHistogram) {
    Registry reg;
    EXPECT_THROW(reg.registerSampled("h", "x", MetricPoint::Type::kHistogram,
                                     [] { return 0.0; }),
                 std::runtime_error);
}

TEST(MetricsTest, SampledInstrumentSerializes) {
    Registry reg;
    reg.registerSampled("cox_loop_iterations_total", "iters",
                        MetricPoint::Type::kCounter, [] { return 42.0; });
    std::string out = expo::prometheus(reg.scrape());
    EXPECT_NE(out.find("# TYPE cox_loop_iterations_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cox_loop_iterations_total 42"), std::string::npos);
}

// ---- Concurrency ----

TEST(MetricsTest, CounterIsThreadSafe) {
    Counter c;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100000;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&c] {
            for (int j = 0; j < kPerThread; ++j) {
                c.inc();
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }
    EXPECT_EQ(c.value(),
              static_cast<uint64_t>(kThreads) * kPerThread);  // no lost updates
}

TEST(MetricsTest, HistogramIsThreadSafe) {
    Histogram h({1.0, 10.0, 100.0});
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50000;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&h] {
            for (int j = 0; j < kPerThread; ++j) {
                h.observe(5.0);  // all land in le=10
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }
    auto s = h.snapshot();
    const uint64_t total = static_cast<uint64_t>(kThreads) * kPerThread;
    EXPECT_EQ(s.count, total);
    EXPECT_DOUBLE_EQ(s.sum, 5.0 * static_cast<double>(total));
    EXPECT_EQ(s.counts.back(), total);  // +Inf cumulative == total
}
