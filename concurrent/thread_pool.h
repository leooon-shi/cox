#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace concurrent {

// A fixed-size worker thread pool with a bounded task queue.
//
// Purpose (see insights/reactor-blocking-work.md): offload blocking or
// CPU-bound work off an event loop thread so a slow operation cannot stall the
// loop and every connection on it. Results are typically posted back to the
// owning EventLoop via runInLoop by the task itself.
//
// The queue is BOUNDED. When it is full:
//     - run()    blocks until space is available (backpressure),
//     - tryRun() returns false immediately (load-shedding signal for overload
//       control).
// A max_queue_size of 0 means unbounded (no backpressure); prefer a bound in
// production so a burst cannot grow the backlog toward OOM.
class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(std::string name = "ThreadPool");
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Maximum number of queued (not-yet-running) tasks. 0 = unbounded.
    // Must be set before start().
    void setMaxQueueSize(size_t size) { max_queue_size_ = size; }

    // Launch num_threads workers. If num_threads == 0 the pool runs tasks
    // synchronously in the caller's thread (useful for tests / degenerate
    // mode).
    void start(int num_threads);

    // Stop the pool: wake all workers, let them finish draining, and join.
    // Idempotent.
    void stop();

    // Enqueue a task. Blocks if the queue is full (bounded mode). If the pool
    // has zero threads, runs the task inline.
    void run(Task task);

    // Try to enqueue without blocking. Returns false if the queue is full
    // (the task is NOT run). With zero threads, runs inline and returns true.
    bool tryRun(Task task);

    // Maximum number of tasks a worker drains per mutex acquisition. Larger
    // values amortize lock/wakeup overhead across many tasks (important for
    // huge, fast-to-run workloads) at the cost of coarser load balancing.
    // Must be >= 1 and set before start(). Default 32.
    void setTakeBatchSize(size_t size) { take_batch_size_ = size; }

    size_t queueSize() const;
    bool running() const { return running_.load(); }
    const std::string& name() const { return name_; }

private:
    void runInThread();
    // Drain up to take_batch_size_ tasks into `out`, blocking until at least
    // one is available or the pool stops. `out` is left empty only when the
    // pool is stopping and the queue is fully drained. Amortizes one mutex
    // acquisition (and condvar wait) over a whole batch of tasks.
    void takeBatch(std::vector<Task>& out);
    bool isFull() const;  // Caller must hold mutex_

    std::string name_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<Task> queue_;
    std::vector<std::thread> threads_;
    size_t max_queue_size_{0};
    size_t take_batch_size_{32};
    std::atomic<bool> running_{false};
};

}  // namespace concurrent
