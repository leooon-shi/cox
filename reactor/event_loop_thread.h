#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace reactor {
class EventLoop;

// EventLoopThread runs an EventLoop in its own thread (the "one loop per
// thread" model). Constructing does not start the thread; startLoop() launches
// it and blocks until the loop is created and running, returning a pointer to
// the loop. The loop is quit and the thread joined on destruction
class EventLoopThread {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;
    explicit EventLoopThread(ThreadInitCallback cb = ThreadInitCallback(),
                             std::string name = std::string());
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    // Start the thread and return the EventLoop it runs. Blocks until ready.
    EventLoop* startLoop();

private:
    void threadFunc();

    EventLoop* loop_{nullptr};  // guarded by mutex_ until initialized
    bool exiting_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    ThreadInitCallback init_callback_;
    std::string name_;
};

}  // namespace reactor
