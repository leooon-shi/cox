#include "event_loop.h"
#include "timestamp.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
namespace
{
// Helper: run a loop in its own thread, execute `setup` inside the loop thread
// once it has started, and stop the loop after `run_for`.
class LoopFixture
{
public:
    LoopFixture()
    {
        thread_ = std::thread([this] {
            reactor::EventLoop loop;
            loop_ = &loop;
            started_.store(true);
            loop.loop();
            loop_ = nullptr;
        });
        while (!started_.load())
        {
            std::this_thread::sleep_for(1ms);
        }
    }
    ~LoopFixture()
    {
        if (loop_)
        {
            loop_->quit();
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    reactor::EventLoop* loop() { return loop_; }

    void quit()
    {
        if (loop_) loop_->quit();
    }

private:
    std::thread thread_;
    std::atomic<bool> started_{false};
    reactor::EventLoop* loop_{nullptr};
};
} // namespace


TEST(TimerTest, RunAfterFiresOnce)
{
    LoopFixture fx;
    std::atomic<int> count{0};
    
    fx.loop()->runAfter(50, [&] { count++; });

    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(count.load(), 1);
}

TEST(TimerTest, RunAfterDoesNotFireEarly)
{
    LoopFixture fx;
    std::atomic<int> count{0};

    fx.loop()->runAfter(300, [&] { count++; });

    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(count.load(), 0);   // not yet elapsed
}

TEST(TimerTest, RunEveryFiresRepeatedly)
{
    LoopFixture fx;
    std::atomic<int> count{0};

    fx.loop()->runEvery(50, [&] { count++; });

    std::this_thread::sleep_for(280ms);  // expect ~5 ticks
    int observed = count.load();
    
    EXPECT_GE(observed, 3);
    EXPECT_LE(observed, 7);
}

TEST(TimerTest, MultipleTimersFireInOrder)
{
    LoopFixture fx;
    std::vector<int> order;
    std::mutex m;

    auto record = [&](int id) {
        std::lock_guard<std::mutex> lock(m);
        order.push_back(id);
    };

    // Schedule out of order; expect firing order 1,2,3 by delay.
    fx.loop()->runAfter(150, [&] { record(3); });
    fx.loop()->runAfter(50, [&] { record(1); });
    fx.loop()->runAfter(100, [&] { record(2); });

    std::this_thread::sleep_for(300ms);

    std::lock_guard<std::mutex> lock(m);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(TimerTest, CancelPreventsFiring)
{
    // Schedule and cancel from within the loop thread so the TimerId is valid.
    std::atomic<int> count{0};
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread t([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;

        loop.runInLoop([&] {
            reactor::TimerId id = loop.runAfter(150, [&] { count++; });
            // Cancel before it fires.
            loop.runAfter(50, [&, id]() mutable { loop.cancel(id); });
        });

        started.store(true);
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(300ms);
    loop_ptr->quit();
    t.join();

    EXPECT_EQ(count.load(), 0);
}

TEST(TimerTest, CrossThreadRunAfter)
{
    LoopFixture fx;
    std::atomic<int> count{0};

    // Called from the test thread (not the loop thread) -> marshalled in.
    std::thread scheduler([&] {
        fx.loop()->runAfter(50, [&] { count++; });
    });
    scheduler.join();

    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(count.load(), 1);
}
