#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace reactor {
class EventLoop;
class EventLoopThread;

// EventLoopThreadPool manages a set of EventLoopThreads for the "one loop per
// thread" concurrency model. A base loop (the acceptor's loop) is provided at
// construction; getNextLoop() round-robins connections across the pool's io
// loops, or returns the base loop when the pool size is zero (single-threaded).
class EventLoopThreadPool {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThreadPool(EventLoop* base_loop, std::string name);
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    // Number of io threads to spawn. 0 (default) => everything runs on the
    // base loop. Must be set before start().
    void setThreadNum(int num_threads) { num_threads_ = num_threads; }

    // Spawn the io threads and start their loops. Idempotent-safe to call once.
    void start(const ThreadInitCallback& cb = ThreadInitCallback());

    // Round-robin the next io loop for a new connection. Returns the base loop
    // when the pool is empty.
    EventLoop* getNextLoop();

    // All loops (base loop when empty, otherwise the io loops).
    std::vector<EventLoop*> getAllLoops();

    bool started() const { return started_; }
    const std::string& name() const { return name_; }

private:
    EventLoop* base_loop_;
    std::string name_;
    bool started_{false};
    int num_threads_{0};
    size_t next_{0};  // round-robin cursor
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};
}  // namespace reactor
