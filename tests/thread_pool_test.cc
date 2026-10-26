#include "thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using concurrent::ThreadPool;

TEST(ThreadPoolTest, RunsAllTasks) {
    ThreadPool pool("t1");
    pool.start(4);

    std::atomic<int> counter{0};
    constexpr int kTasks = 1000;
    for (int i = 0; i < kTasks; ++i) {
        pool.run([&counter] { counter++; });
    }
    pool.stop();  // drains remaining tasks before joining
    EXPECT_EQ(counter.load(), kTasks);
}

// A large burst of trivial tasks: workers drain in batches to amortize the
// mutex (one lock per batch, not per task). Correctness must be unaffected --
// every task runs exactly once.
TEST(ThreadPoolTest, RunsLargeBurstOfTrivialTasks) {
    ThreadPool pool("burst");
    pool.start(10);

    std::atomic<int> counter{0};
    constexpr int kTasks = 500000;
    for (int i = 0; i < kTasks; ++i) {
        pool.run(
            [&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.stop();  // drains everything before joining
    EXPECT_EQ(counter.load(), kTasks);
}

// Batching must not starve idle workers: a handful of slow tasks submitted to
// a multi-worker pool should run in parallel, not be hoarded by one worker and
// run back-to-back. With N slow tasks and >=N workers, wall time stays close to
// a single task's duration rather than N times it.
TEST(ThreadPoolTest, SlowTasksStayParallelUnderBatching) {
    ThreadPool pool("fair");
    pool.setTakeBatchSize(
        32);  // large batch; fair-share must still spread work
    pool.start(4);

    std::atomic<int> done{0};
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; ++i) {
        pool.run([&done] {
            std::this_thread::sleep_for(100ms);
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    // Wait for all four to finish.
    for (int i = 0; i < 400 && done.load() < 4; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    pool.stop();

    EXPECT_EQ(done.load(), 4);
    // Parallel: ~100ms. Serialized (one worker hoarding all 4): ~400ms.
    EXPECT_LT(elapsed, 250);
}

TEST(ThreadPoolTest, TasksRunOnWorkerThreads) {
    ThreadPool pool("t2");
    pool.start(2);

    std::atomic<bool> ran{false};
    std::thread::id worker_tid;
    pool.run([&] {
        worker_tid = std::this_thread::get_id();
        ran = true;
    });
    for (int i = 0; i < 100 && !ran.load(); ++i) {
        std::this_thread::sleep_for(2ms);
    }
    pool.stop();
    EXPECT_TRUE(ran.load());
    EXPECT_NE(worker_tid, std::this_thread::get_id());
}

TEST(ThreadPoolTest, ZeroThreadsRunsInline) {
    ThreadPool pool("t3");
    pool.start(0);  // no workers

    std::atomic<int> counter{0};
    std::thread::id run_tid;
    pool.run([&] {
        run_tid = std::this_thread::get_id();
        counter++;
    });
    // Ran synchronously in this thread.
    EXPECT_EQ(counter.load(), 1);
    EXPECT_EQ(run_tid, std::this_thread::get_id());
    pool.stop();
}

TEST(ThreadPoolTest, TryRunRejectsWhenFull) {
    ThreadPool pool("t4");
    pool.setMaxQueueSize(2);
    pool.start(1);

    // Block the single worker so the queue fills up and stays full.
    std::atomic<bool> release{false};
    pool.run([&] {
        while (!release.load()) std::this_thread::sleep_for(1ms);
    });
    // Give the worker a moment to pick up the blocking task.
    std::this_thread::sleep_for(20ms);

    // Fill the bounded queue (capacity 2).
    EXPECT_TRUE(pool.tryRun([] {}));
    EXPECT_TRUE(pool.tryRun([] {}));
    // Now the queue is full -> further tryRun must be rejected.
    EXPECT_FALSE(pool.tryRun([] {}));

    release = true;
    pool.stop();
}

TEST(ThreadPoolTest, RunBlocksWhenFullThenProceeds) {
    ThreadPool pool("t5");
    pool.setMaxQueueSize(1);
    pool.start(1);

    std::atomic<bool> release{false};
    std::atomic<int> done{0};

    // Occupy the worker.
    pool.run([&] {
        while (!release.load()) std::this_thread::sleep_for(1ms);
        done++;
    });
    std::this_thread::sleep_for(20ms);

    // Queue one (fills capacity-1 queue).
    pool.run([&] { done++; });

    // This run() should BLOCK because the queue is full; release the worker on
    // another thread so it can proceed.
    std::thread releaser([&] {
        std::this_thread::sleep_for(50ms);
        release = true;
    });

    auto start = std::chrono::steady_clock::now();
    pool.run([&] { done++; });  // block until a slot frees
    auto waited = std::chrono::steady_clock::now() - start;

    releaser.join();
    pool.stop();

    EXPECT_EQ(done.load(), 3);
    // It should have waited a meaningful amount (worker was released at ~50ms).
    EXPECT_GE(
        std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(),
        20);
}

TEST(ThreadPoolTest, DrainsQueuedTasksOnStop) {
    ThreadPool pool("t6");
    pool.start(2);

    std::atomic<int> counter{0};
    // Enqueue tasks that each take a little time, then stop immediately.
    constexpr int kTasks = 200;
    for (int i = 0; i < kTasks; ++i) {
        pool.run([&counter] {
            std::this_thread::sleep_for(1ms);
            counter++;
        });
    }
    pool.stop();  // must drain all queued tasks before returning
    EXPECT_EQ(counter.load(), kTasks);
}

TEST(ThreadPoolTest, ConcurrentProducers) {
    ThreadPool pool("t7");
    pool.start(4);

    std::atomic<int> counter{0};
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 500;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                pool.run([&counter] { counter++; });
            }
        });
    }

    for (auto& t : producers) t.join();
    pool.stop();
    EXPECT_EQ(counter.load(), kProducers * kPerProducer);
}
