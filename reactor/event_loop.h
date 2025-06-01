#pragma once

#include "thread/current_thread.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>
#include <thread>
#include <cassert>

namespace logging
{
    class Logger;
}
namespace reactor {
class Poller;
class Channel;
// class TimerHeap;

class EventLoop {
public:
    using Task = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    void runInLoop(Task && task);
    void runAfter(uint64_t delay_ms, Task task);
    void runEvery(uint64_t interval_ms, Task task);
    inline bool isInLoopThread() const { return current_thread::tid() == loop_thread_id_;}
    void assertInLoopThread() const;

private:
    void queueInLoop(Task && task);
    void wakeup();
    void handleWakeup();
    void doPendingTasks();

    std::mutex task_mutex_;

    std::unique_ptr<logging::Logger> logger_;

    std::unique_ptr<Poller> poller_;
    std::unique_ptr<Channel> wakeup_channel_;
    //std::unique_ptr<TimerHeap> timer_heap_;

    const pid_t loop_thread_id_;

    int wakeup_fd_{-1};
    int writeup_fd_{-1};

    std::atomic<bool> quit_{false};
    // It would be better to combine this with a light std::atomic<Channel*> if needed crossed-thread.
    // At present, only the loop thread accesses this, then it's fine as-is.
    Channel * processing_channel_ {nullptr};
    std::queue<Task> pending_tasks_;
};
} //namespace reactor