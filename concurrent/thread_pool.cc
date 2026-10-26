#include "thread_pool.h"

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

namespace concurrent
{
ThreadPool::ThreadPool(std::string name)
: name_(std::move(name)) {}

ThreadPool::~ThreadPool()
{
    if (running_.load())
    {
        stop();
    }
}

void ThreadPool::start(int num_threads)
{
    assert(!running_.load());
    running_ = true;
    threads_.reserve(static_cast<size_t>(num_threads));
    for (int i = 0; i < num_threads; ++i)
    {
        threads_.emplace_back([this] { runInThread(); });
    }
    // num_threads == 0 => tasks run inline in run()/tryRun().
}

void ThreadPool::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) {
            return;
        }
        running_ = false;
    }

    // Wake all workers (both those waiting for tasks and any producer waiting
    // for space) so they can observe the stopped state and exit.
    not_empty_.notify_all();
    not_full_.notify_all();
    for (auto& t : threads_)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    threads_.clear();
}

bool ThreadPool::isFull() const
{
    return max_queue_size_ > 0 && queue_.size() >= max_queue_size_;
}

void ThreadPool::run(Task task)
{
    if (threads_.empty())
    {
        task();  // no workers: run inline
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    // Block while the queue is full and the pool is still running.
    not_full_.wait(lock, [this] { return !isFull() || !running_.load(); });
    if (!running_.load())
    {
        return;   // dropped: pool is stopping
    }
    queue_.push_back(std::move(task));
    not_empty_.notify_one();
}

bool ThreadPool::tryRun(Task task)
{
    if (threads_.empty())
    {
        task();     // No workers: run inline
        return true;
    }
    
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_.load())
    {
        return false;
    }

    if (isFull())
    {
        return false;   // load-shedding: caller should reject/deny
    }
    queue_.push_back(std::move(task));
    not_empty_.notify_one();

    return true;
}

void ThreadPool::takeBatch(std::vector<Task>& out) 
{
    out.clear();
    std::unique_lock<std::mutex> lock(mutex_);
    // Wait for a task, or for the pool to stop.
    not_empty_.wait(lock,
                    [this]{ return !queue_.empty() || !running_.load(); });

    if (queue_.empty())
    {
        // Pool is stopping and drained: leave `out` empty so the worker exits.
        return;
    }
    // Capture capacity state *before* draining. A producer can only be blocked
    // in run() when the queue was at the bound.
    const bool was_full = isFull();
    // Take at most a fair share of the backlog so slow tasks do not starve
    // idle workers: if one worker greedily drained the whole queue, the others
    // would sit idle while it ran the batch sequentially. Dividing by the
    // worker count keeps work spreadable while still amortizing the mutex over
    // many tasks on deep queues. Capped by take_batch_size_ and >= 1.
    const size_t workers = threads_.empty() ? 1 : threads_.size();
    const size_t fair_share = (queue_.size() + workers - 1) / workers;
    const size_t cap = take_batch_size_ > 0 ? take_batch_size_ : 1;
    const size_t n = std::min({cap, fair_share, queue_.size()});
    out.reserve(n);

    for (size_t i = 0; i < n; ++i)
    {
        out.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }

    if (was_full)
    {
        // We freed up to `n` slots; wake producers that may be blocked in
        // run(). notify_all because more than one slot opened.
        not_full_.notify_all();
    }
}

void ThreadPool::runInThread() {
    std::vector<Task> batch;
    for (;;)
    {
        takeBatch(batch);
        if (batch.empty())
        {
            // Empty batch is only returned when the pool is stopping and the
            // queue has been fully drained.
            break;
        }
        for (Task& task : batch)
        {
            task();     // Exceptions are the task's responsibility
            task = nullptr;
        }
    }
}

size_t ThreadPool::queueSize() const 
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}
}   // namespace concurrent