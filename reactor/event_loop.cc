#include "event_loop.h"
#include "file/file_opts.h"
#include "io_channel.h"
#include "logger.h"
#include "poller/io_poller.h"
#include "poller/io_poller_factory.h"
#include "timer_queue.h"

#include <unistd.h>  // for ::read, ::write, ::close
#include <cstdint>   // for uint64_t
#include <sstream>   // for std::ostringstream (hex formatting of pointer)

namespace reactor {

// Poll timeout policy.
//
// All real wakeup sources are fds (socket I/O, timerfd for timers, eventfd for
// cross-thread posts), so functionally the loop could block forever. The
// timeout is NOT the timer mechanism -- timers fire precisely via the timerfd.
//
//  - Debug builds block indefinitely (-1). A missed wakeup then surfaces as an
//    obvious hang during development, making the bug easy to catch.
//  - Release builds use a bounded LIVENESS BACKSTOP: if any wakeup path were
//    ever missed, poll(-1) would hang the service permanently, which is
//    unacceptable in production. A bounded timeout degrades that worst case to
//    at most kPollTimeoutMs of latency (self-healing) at negligible idle cost.
//
// See insights/poll-timeout-strategy.md for the full rationale.
#if defined(DEBUG)
static constexpr int kPollTimeoutMs = -1;  // block forever (dev visibility)
#else
static constexpr int kPollTimeoutMs = 10000;  // 10s backstop (prod safety)
#endif

#if defined(DEBUG)
EventLoop::EventLoop(std::unique_ptr<Poller> poller)
    : logger_(std::make_unique<logging::Logger>()),
      poller_(std::move(poller)),
      loop_thread_id_(current_thread::tid()) {
    wakeup_fd_ = file_opts::createEventfd();
    std::exchange(wakeup_channel_, std::make_unique<Channel>(wakeup_fd_, this));

    wakeup_channel_->setReadCallback([this] { handleWakeup(); });

    std::ostringstream oss;
    oss << std::hex << reinterpret_cast<std::uintptr_t>(wakeup_channel_.get());

    logger_->info("EventLoop created with custom poller, wakeup channel 0x" +
                  oss.str());
}
#endif  // DEBUG

EventLoop::EventLoop()
    : logger_(std::make_unique<logging::Logger>()),
      poller_(PollerFactory::create()),
      loop_thread_id_(current_thread::tid()) {
    wakeup_fd_ = file_opts::createEventfd();

    std::exchange(wakeup_channel_, std::make_unique<Channel>(wakeup_fd_, this));

    wakeup_channel_->setReadCallback([this] { handleWakeup(); });

    wakeup_channel_->enableRead();

    // TimerQueue registers its own timerfd Channel with this loop's poller.
    timer_queue_ = std::make_unique<TimerQueue>(this);

    logger_->info("EventLoop created, wakeup_fd: " +
                  std::to_string(wakeup_fd_));
}

EventLoop::~EventLoop() {
    assert(quit_);

    // Tear down the timer queue first: it removes its timerfd channel from the
    // poller while this loop is still fully alive.
    timer_queue_.reset();

    wakeup_channel_->disableAll();

    poller_->removeChannel(wakeup_channel_.get());

    wakeup_channel_.reset();

    // Invalidate the fd under the same lock wakeup() uses, THEN close it. A
    // concurrent wakeup() from another thread (quit()/queueInLoop()) will see
    // -1 and skip the write, rather than writing into a descriptor number the
    // OS may already have handed to something else.
    int fd_to_close = -1;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        fd_to_close = wakeup_fd_;
        wakeup_fd_ = -1;
    }

    if (fd_to_close >= 0) {
        ::close(fd_to_close);
    }

    logger_->info("~EventLoop");
}

void EventLoop::loop() {
#ifdef DEBUG
    assertInLoopThread();
#endif

    while (!quit_) {
        // Timeout policy is defined by kPollTimeoutMs (see top of file):
        // -1 (block forever) in Debug, a bounded backstop in Release.
        active_channels_.clear();
        poller_->poll(kPollTimeoutMs, active_channels_);

        for (auto* channel : active_channels_) {
            processing_channel_.store(channel, std::memory_order_relaxed);
            channel->handleEvents();
        }

        // Reset processing channel to avoid meaningless wakeups.
        processing_channel_.store(nullptr, std::memory_order_relaxed);

        doPendingTasks();
    }
    logger_->info("loop end.");
}

void EventLoop::runInLoop(Task&& task) {
    if (isInLoopThread()) {
        // logger_->info("isInLoopThread");
        task();  // run immediately in the loop
    } else {
        // logger_->info("queueInLoop");
        queueInLoop(std::forward<Task>(task));
    }
}

void EventLoop::queueInLoop(Task&& task) {
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        pending_tasks_.emplace(std::forward<Task>(task));
    }

    // Wake up if the caller is not in the loop thread, or if the loop is
    // currently dispatching channel events (so the newly queued task is not
    // delayed until the next poll wakeup).
    if (!isInLoopThread() ||
        processing_channel_.load(std::memory_order_relaxed)) {
        wakeup();
    }
}

TimerId EventLoop::runAt(logging::Timestamp when, Task task) {
    // addTimer mutates the queue and must run in the loop thread. When called
    // from another thread we marshal the scheduling in, but the returned
    // TimerId is only meaningful when called from the loop thread. Callers that
    // need to cancel should schedule from the loop thread.
    if (isInLoopThread()) {
        return timer_queue_->addTimer(std::move(task), when, 0);
    }

    auto shared_task = std::make_shared<Task>(std::move(task));
    queueInLoop([this, when, shared_task] {
        timer_queue_->addTimer(std::move(*shared_task), when, 0);
    });

    return TimerId();
}

TimerId EventLoop::runAfter(uint64_t delay_ms, Task task) {
    logging::Timestamp when =
        logging::addMilliseconds(logging::Timestamp::now(), delay_ms);
    return runAt(when, std::move(task));
}

TimerId EventLoop::runEvery(uint64_t interval_ms, Task task) {
    logging::Timestamp when =
        logging::addMilliseconds(logging::Timestamp::now(), interval_ms);
    if (isInLoopThread()) {
        return timer_queue_->addTimer(std::move(task), when, interval_ms);
    }

    auto shared_task = std::make_shared<Task>(std::move(task));
    queueInLoop([this, when, interval_ms, shared_task] {
        timer_queue_->addTimer(std::move(*shared_task), when, interval_ms);
    });

    return TimerId();
}

void EventLoop::cancel(TimerId id) {
    if (isInLoopThread()) {
        timer_queue_->cancel(id);
    } else {
        queueInLoop([this, id] { timer_queue_->cancel(id); });
    }
}

// A channel must only be updated on the loop thread that owns it. Updating a
// channel belonging to another event loop is a programming error.
void EventLoop::updateChannel(Channel* channel) {
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        logger_->error("EventLoop::assertInLoopThread - current thread id: " +
                       std::string(current_thread::tidStr()) +
                       ", loop thread id: " + std::to_string(loop_thread_id_));
        throw std::runtime_error("EventLoop is not in the loop thread");
    }
}

void EventLoop::removeChannel(Channel* channel) {
    assertInLoopThread();

    if (channel == nullptr) {
        logger_->error("removeChannel - channel is null");
        return;
    }

    // If the channel currently being dispatched is being removed, clear the
    // pointer so a concurrent queueInLoop won't treat it as "still processing".
    if (channel == processing_channel_.load(std::memory_order_relaxed)) {
        processing_channel_.store(nullptr, std::memory_order_relaxed);
    }

    poller_->removeChannel(channel);
}

void EventLoop::quit() {
    quit_ = true;
    // Wakeup when caller is not in loop thread. At worst,
    if (!isInLoopThread()) {
        wakeup();  // provide a chance to handle pending tasks.
    }
}
void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = -1;
    {
        // Hold task_mutex_ so the fd cannot be closed by ~EventLoop between the
        // validity check and the write. wakeup_fd_ is set to -1 under the same
        // lock during teardown, so a late wakeup from another thread becomes a
        // no-op instead of writing to a closed (possibly recycled) descriptor.

        std::lock_guard<std::mutex> lock(task_mutex_);
        if (wakeup_fd_ < 0) {
            return;  // loop is being torn down; nothing to wake
        }

        n = ::write(wakeup_fd_, &one, sizeof(one));
    }

    if (n != sizeof(one)) {
        logger_->error("EventLoop::wakeup writes " + std::to_string(n) +
                       " bytes instead of 8");
    }
}

void EventLoop::handleWakeup() {
    uint64_t one = 0;
    // A single read drains the eventfd counter.
    ssize_t n = ::read(wakeup_fd_, &one, sizeof(one));

    if (n != sizeof(one)) {
        logger_->error("EventLoop::handleWakeup reads " + std::to_string(n) +
                       " bytes instead of 8");
    }
}

void EventLoop::doPendingTasks() {
    std::queue<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        tasks.swap(pending_tasks_);
    }

    while (!tasks.empty()) {
        try {
            tasks.front()();
        } catch (const std::exception& e) {
            logger_->error(std::string("exception in pending task: ") +
                           e.what());
        } catch (...) {
            logger_->error("unknown exception in pending task");
        }

        tasks.pop();
    }
}

}  // namespace reactor