#include "event_loop.h"
#include "io_channel.h"
#include "poller/io_poller.h"
#include "poller/io_poller_factory.h"
#include "logger.h"

#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <utility>

namespace reactor {
namespace {

void setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

} // namespace

EventLoop::EventLoop()
:logger_(std::make_unique<logging::Logger>()),
poller_(PollerFactory::create()),
loop_thread_id_(current_thread::tid())
{
    int fds[2];
    if (::pipe(fds) != 0) {
        std::cerr << "pipe() failed: " << std::strerror(errno) << std::endl;
        std::abort();
    }

    wakeup_fd_ = fds[0];
    writeup_fd_ = fds[1];

    std::exchange(wakeup_channel_, std::make_unique<Channel>(wakeup_fd_, this));
    setNonBlocking(wakeup_fd_);
    setNonBlocking(writeup_fd_);
    
    wakeup_channel_->setReadCallback([this] { handleWakeup(); });
    wakeup_channel_->enableRead();
}

EventLoop::~EventLoop() {
    assert(quit_);
    wakeup_channel_->disableAll();
    poller_->removeChannel(wakeup_channel_.get());
    wakeup_channel_.reset();

    if (wakeup_fd_ >= 0)
    {
        ::close(wakeup_fd_);
    }
    
    if (writeup_fd_ >= 0 && writeup_fd_ != wakeup_fd_)
    {
        ::close(writeup_fd_);
    } 
    logger_->info("~EventLoop");
}

void EventLoop::loop() {
#ifdef DEBUG
    assertInLoopThread();
#endif
    while (!quit_) {
        //int timeout_ms = timer_heap_->getNextTimeout();
        int timeout_ms = 50; //milli seconds
        std::vector<Channel*> active_channels;
        poller_->poll(timeout_ms, active_channels);

        if (active_channels.empty())
        {
            continue;
        }

        for (auto* channel : active_channels) {
            //Race condition might happen, processing_channel_ only used for wakeup, so it's fine as-is.
            processing_channel_ = channel;
            channel->handleEvents();
            logger_->info("after poll, size of channels " + std::to_string(active_channels.size()));
        }
        //reset processing channel to nullptr to avoid meaningless wakeup
        processing_channel_ = nullptr;
        //timer_heap_->expireDueTasks();
        doPendingTasks();
    }
    logger_->info("loop end.");
}

void EventLoop::runInLoop(Task && task) {
    if (isInLoopThread())
    {
        // logger_->info("isInLoopThread");
        task(); //run immediately in the loop
    }
    else
    {
        // logger_->info("queueInLoop");
        queueInLoop(std::forward<Task>(task));
    }
}

void EventLoop::queueInLoop(Task && task)
{
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        pending_tasks_.emplace(std::forward<Task>(task));
    }
    // wakeup only when if invocation is not in loop thread or loop is working on active channels returned by poller.
    if ( !isInLoopThread()|| processing_channel_)
    {
        wakeup();
    }
}

void EventLoop::runAfter(uint64_t delay_ms, Task task) {
    //timer_heap_->addTimer(delay_ms, std::move(task), /*repeating=*/false);
}

void EventLoop::runEvery(uint64_t interval_ms, Task task) {
    //timer_heap_->addTimer(interval_ms, std::move(task), /*repeating=*/true);
}

void EventLoop::updateChannel(Channel* channel) {
    poller_->updateChannel(channel);
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        logger_->error("EventLoop::assertInLoopThread - current thread id: " + std::string(current_thread::tidStr())+
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
    //FXIME race condition might happen, if the channel is being processed by handleEvents.
    // Before poller returned, It's secured to remove it from poller.
    // Ongongin events handlings, and the channel is in ActiveChannels. 
    // if application removed it directly, it might cause a coredump, Channel pointer might be invalid at that moment.
    // So, we should remove it from poller after the channel is handled.
    if (channel == processing_channel_) {
        processing_channel_ = nullptr; // reset processing channel to avoid meaningless wakeup
    }
    poller_->removeChannel(channel);
}

void EventLoop::quit() {
    quit_ = true;
    //Wakeup when caller is not in loop thread. At worst, 
    if (!isInLoopThread())
    {
        wakeup();//provide a chance to handle pending tasks.
    }
}

void EventLoop::wakeup() {
    // logger_->info("wakeup");
    uint64_t one = 1;
    (void)::write(writeup_fd_, &one, sizeof(one));
    // logger_->info("wakeup done");
}

void EventLoop::handleWakeup() {
    uint64_t one;
    while (::read(wakeup_fd_, &one, sizeof(one)) > 0) {}
}

void EventLoop::doPendingTasks() {
    std::queue<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        tasks.swap(pending_tasks_);
    }

    while (!tasks.empty()) {
        tasks.front()();
        tasks.pop();
        logger_->info("handling pending task..");
    }
}

} //namespace reactor