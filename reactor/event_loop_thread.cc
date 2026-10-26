#include "event_loop_thread.h"

#include "event_loop.h"

#include <utility>

namespace reactor {
EventLoopThread::EventLoopThread(ThreadInitCallback cb, std::string name)
    : init_callback_(std::move(cb)), name_(std::move(name)) {}

EventLoopThread::~EventLoopThread() {
    exiting_ = true;
    if (loop_ != nullptr) {
        // quit() is thread-safe: it wakes the loop via the eventfd.
        loop_->quit();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

EventLoop* EventLoopThread::startLoop() {
    thread_ = std::thread([this] { threadFunc(); });

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return loop_ != nullptr; });
        loop = loop_;
    }

    return loop;
}

void EventLoopThread::threadFunc() {
    // The EventLoop must be created in *this* thread so its loop_thread_id_
    // matches the running thread.
    EventLoop loop;

    if (init_callback_) {
        init_callback_(&loop);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }

    loop.loop();  // runs until quit()

    // Loop has exited; clear the pointer so the destructor won't touch a stale
    // stack object.
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
}

}  // namespace reactor
