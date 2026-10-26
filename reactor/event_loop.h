#pragma once

#include "thread/current_thread.h"
#include "timer.h"
#include "timestamp.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace logging {
class Logger;
}

namespace reactor {
class Poller;
class Channel;
class TimerQueue;

class EventLoop {
public:
    using Task = std::function<void()>;
#if defined(DEBUG)
    EventLoop(std::unique_ptr<Poller> poller);
    const Poller& getPoller() const { return *poller_; }
    Poller& getPoller() { return *poller_; }
#endif
    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    void runInLoop(Task&& task);

    // Queue a task to run on a LATER loop iteration, never inline -- even when
    // called from the loop thread. Use this (not runInLoop) to defer destroying
    // an object from inside its own callback: runInLoop executes immediately
    // when already on the loop thread, which would free the object while its
    // caller is still using it. (A real use-after-free found this way; see
    // Connector::removeAndResetChannel.)
    void queueInLoop(Task&& task);

    // Timer API. Callbacks run in the loop thread. These are safe to call from
    // any thread; scheduling is marshalled into the loop thread as needed.
    TimerId runAt(logging::Timestamp when, Task task);
    TimerId runAfter(uint64_t delay_ms, Task task);
    TimerId runEvery(uint64_t interval_ms, Task task);
    void cancel(TimerId id);

    inline bool isInLoopThread() const {
        return current_thread::tid() == loop_thread_id_;
    }
    void assertInLoopThread() const;

private:
    void wakeup();
    void handleWakeup();
    void doPendingTasks();

    std::mutex task_mutex_;

    std::unique_ptr<logging::Logger> logger_;

    std::unique_ptr<Poller> poller_;
    std::unique_ptr<TimerQueue> timer_queue_;
    std::unique_ptr<Channel> wakeup_channel_;

    const pid_t loop_thread_id_;

    // Single Linux eventfd used for cross-thread wakeup.
    //
    // THREAD SAFETY: wakeup() may be called from any thread (via quit() or
    // queueInLoop()), while ~EventLoop() closes the fd on the loop thread.
    // Writing to a closed fd is not merely useless -- the number can be
    // recycled by the OS, so the write could land on an unrelated socket.
    // wakeup_fd_ is therefore guarded by task_mutex_ (already taken on the
    // cross-thread path) and set to -1 before being closed, so a concurrent
    // wakeup() sees the teardown and does nothing. Found by ThreadSanitizer.
    int wakeup_fd_{-1};

    std::atomic<bool> quit_{false};
    // Written by the loop thread, read by other threads in queueInLoop to
    // decide whether a wakeup is needed. Atomic to avoid a data race.
    std::atomic<Channel*> processing_channel_{nullptr};
    std::queue<Task> pending_tasks_;

    // Reused across loop iterations to avoid per-iteration heap allocation.
    std::vector<Channel*> active_channels_;
};
}  // namespace reactor
