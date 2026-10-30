// Micro-benchmark for concurrent::ThreadPool task throughput.
//
// The dominant cost of a worker pool on a huge, fast-to-run workload is mutex
// contention: a naive pool locks the queue once per task, so N tasks cost N
// lock/unlock cycles across the workers. ThreadPool amortizes this by draining
// a *batch* of tasks per lock acquisition (setTakeBatchSize). This benchmark
// sweeps the batch size so the effect is visible as numbers rather than
// asserted in prose.
//
// Workload: a large burst of trivial tasks (a single relaxed atomic increment).
// Trivial tasks are the adversarial case for a pool -- the per-task work is far
// cheaper than the synchronization around it, so any lock overhead dominates
// the measurement. Real tasks do more and will show smaller relative gains.

#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;
using concurrent::ThreadPool;

namespace {
// Describe the build/runtime environment so a results table is interpretable on
// its own -- benchmark numbers are machine-, compiler- and build-dependent, and
// a table with no provenance is a table you cannot trust or compare against.
std::string compilerId() {
#if defined(__clang__)
    return "clang++ " __clang_version__;
#elif defined(__GNUC__)
    return "g++ " __VERSION__;
#else
    return "unknown compiler";
#endif
}

std::string buildType() {
#if defined(NDEBUG)
    return "Release (NDEBUG)";
#else
    return "Debug";
#endif
}

void printEnv() {
    const unsigned hw = std::thread::hardware_concurrency();
    std::time_t now = std::time(nullptr);
    char ts[32] = {};
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%SZ", std::gmtime(&now));

    std::cout << "--- environment ---\n";
    std::cout << "date:        " << ts << "\n";
    std::cout << "compiler:    " << compilerId() << "\n";
    std::cout << "build:       " << buildType() << "\n";
    std::cout << "C++ std:     " << __cplusplus << "\n";
    std::cout << "hw threads:  "
              << (hw ? std::to_string(hw) : std::string("unknown")) << "\n";
    if (buildType().rfind("Debug", 0) == 0) {
        std::cout << "WARNING:     Debug build (-O0) -- numbers are not "
                     "representative; use a Release build.\n";
    }
    std::cout << "-------------------\n\n";
}

// Run `num_tasks` trivial tasks through a pool with `num_threads` workers and
// the given batch size, enqueuing from `num_producers` threads in parallel.
//
// Multiple producers matter: with a single producer the enqueue side (one lock
// per task) is the bottleneck and the queue stays near-empty, so workers rarely
// have a batch to drain and batch size barely moves the number. Several
// producers keep the queue backed up, which is the regime where dequeue
// batching actually pays off -- and the regime a real "burst of 100M tasks"
// workload is in.
double runOnce(int num_threads, size_t batch_size, int num_tasks,
               int num_producers) {
    std::atomic<int64_t> counter{0};

    ThreadPool pool("bench");
    pool.setTakeBatchSize(batch_size);
    // Unbounded queue: we are measuring dequeue/lock cost, not backpressure.
    pool.start(num_threads);

    auto producer = [&](int tasks) {
        for (int i = 0; i < tasks; ++i) {
            pool.run([&counter] {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
    };

    auto t0 = steady_clock::now();
    std::vector<std::thread> producers;
    producers.reserve(static_cast<size_t>(num_producers));
    const int per_producer = num_tasks / num_producers;
    const int remainder = num_tasks % num_producers;

    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back(producer,
                               per_producer + (p < remainder ? 1 : 0));
    }
    for (auto& t : producers) {
        t.join();
    }
    pool.stop();  // drains all queued tasks before joining
    auto t1 = steady_clock::now();

    if (counter.load() != num_tasks) {
        std::cerr << "ERROR: ran " << counter.load() << " of " << num_tasks
                  << " tasks\n";
        std::exit(1);
    }

    return duration_cast<duration<double>>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    // thread_pool_bench [threads] [num_tasks] [producers]
    const int threads = argc > 1 ? std::atoi(argv[1]) : 10;
    const int num_tasks = argc > 2 ? std::atoi(argv[2]) : 10'000'000;
    const int producers = argc > 3 ? std::atoi(argv[3]) : 4;

    std::cout << "=== Cox thread pool benchmark ===\n";
    printEnv();
    std::cout << "worker threads: " << threads << "\n";
    std::cout << "producers:      " << producers << "\n";
    std::cout << "tasks:          " << num_tasks
              << " (each: one relaxed atomic increment)\n\n";
    std::cout << "batch     elapsed(s)   tasks/s        speedup\n";
    std::cout << "----      ----------   -------        -------\n";

    const std::vector<size_t> batch_sizes = {1, 2, 4, 8, 16, 32, 64, 128};
    double baseline_s = 0.0;  // batch-1 is the one-lock-per-task reference
    for (size_t batch : batch_sizes) {
        double s = runOnce(threads, batch, num_tasks, producers);
        if (batch == 1) {
            baseline_s = s;
        }
        double tasks_per_s = static_cast<double>(num_tasks) / s;
        double speedup = baseline_s > 0.0 ? baseline_s / s : 1.0;
        std::printf("%5zu   %10.4f   %12.0f %6.2fx\n", batch, s, tasks_per_s,
                    speedup);
    }

    std::cout << "\nNote: batch=1 is one dequeue lock per task (naive pool); "
                 "higher batch sizes\namortize the dequeue lock across many "
                 "tasks. Speedup is relative to batch=1.\n";
    return 0;
}
