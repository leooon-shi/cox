#include "event_loop.h"
#include "event_loop_thread.h"
#include "event_loop_thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

using namespace std::chrono_literals;
using reactor::EventLoop;
using reactor::EventLoopThread;
using reactor::EventLoopThreadPool;

TEST(EventLoopThreadTest, StartLoopRunsTaskInThread)
{
    EventLoopThread thread;
    EventLoop* loop = thread.startLoop();
    ASSERT_NE(loop, nullptr);

    std::atomic<bool> ran{false};
    std::thread::id run_tid;
    loop->runInLoop([&]() {
        run_tid = std::this_thread::get_id();
        ran = true;
    });

    for (int i = 0; i < 100 && !ran.load(); ++i)
    {
        std::this_thread::sleep_for(2ms);
    }

    EXPECT_TRUE(ran.load());
    // The task ran in the loop's own thread, not the test thread.
    EXPECT_NE(run_tid, std::this_thread::get_id());
    // Destructor quits and joins.
}

TEST(EventLoopThreadPoolTest, ZeroThreadUsesBaseLoop)
{
    EventLoop base;
    // start() and getNextLoop() must be called on the base loop thread.
    std::atomic<bool> done{false};
    base.runInLoop([&]() {
        EventLoopThreadPool pool(&base, "pool0");
        pool.setThreadNum(0);
        pool.start();
        EXPECT_EQ(pool.getNextLoop(), &base);
        EXPECT_EQ(pool.getNextLoop(), &base);
        done = true;
        base.quit();
    });
    base.loop();
    EXPECT_TRUE(done.load());
}

TEST(EventLoopThreadPoolTest, RoundRobinAcrossLoops)
{
    EventLoop base;
    std::atomic<bool> done{false};

    base.runInLoop([&]() {
        EventLoopThreadPool pool(&base, "pool3");
        pool.setThreadNum(3);
        pool.start();

        auto loops = pool.getAllLoops();
        ASSERT_EQ(loops.size(), 3u);

        // getNextLoop cycles through the three io loops in order and wraps.
        EventLoop* a = pool.getNextLoop();
        EventLoop* b = pool.getNextLoop();
        EventLoop* c = pool.getNextLoop();
        EventLoop* d = pool.getNextLoop();          // wraps to first

        std::set<EventLoop*> distinct{a, b, c};
        EXPECT_EQ(distinct.size(), 3u);         // three distinct loops
        EXPECT_NE(a, &base);                    // none is the base loop
        EXPECT_EQ(d, a);                        // wrapped around

        done = true;
        base.quit();
    });

    base.loop();
    EXPECT_TRUE(done.load());
}