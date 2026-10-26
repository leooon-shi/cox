#include "event_loop_thread_pool.h"

#include "event_loop.h"
#include "event_loop_thread.h"

#include <cassert>
#include <utility>

namespace reactor {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, std::string name)
    : base_loop_(base_loop), name_(std::move(name)) {}

EventLoopThreadPool::~EventLoopThreadPool()
{
    // EventLoopThread destructors quit and join their loops.
}

void EventLoopThreadPool::start(const ThreadInitCallback& cb)
{
    assert(!started_);
    base_loop_->assertInLoopThread();

    started_ = true;

    for (int i = 0; i < num_threads_; ++i)
    {
        std::string name = name_ + "-io" + std::to_string(i);
        auto t = std::make_unique<EventLoopThread>(cb, name);
        loops_.push_back(t->startLoop());
        threads_.push_back(std::move(t));
    }

    // Single-threaded mode: run the init callback on the base loop so callers
    // see consistent behavior regardless of thread count.
    if (num_threads_ == 0 && cb)
    {
        cb(base_loop_);
    }
}

EventLoop* EventLoopThreadPool::getNextLoop()
{
    base_loop_->assertInLoopThread();
    assert(started_);
    if (loops_.empty())
    {
        return base_loop_;
    }

    EventLoop* loop = loops_[next_];
    next_ = (next_ + 1) % loops_.size();
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
    assert(started_);
    if (loops_.empty())
    {
        return {base_loop_};
    }

    return loops_;
}

}   // namespace reactor
