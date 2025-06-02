#include "event_loop.h"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>

TEST(EventLoopTest, TaskScheduling) {
    reactor::EventLoop loop;
    std::atomic<int> counter = 0;

    // Schedule a task
    loop.runInLoop([&]() {
        counter++;
    });

    // Run the event loop in a separate thread
    std::thread loopThread([&]() {
        loop.loop();
    });

    // Allow some time for the task to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    loop.quit();
    loopThread.join();

    // Verify the task was executed
    EXPECT_EQ(counter.load(), 1);
}

TEST(EventLoopTest, Concurrency) {
    reactor::EventLoop loop;
    std::atomic<int> counter = 0;

    // Schedule tasks from multiple threads
    std::thread t1([&]() {
        for (int i = 0; i < 100; ++i) {
            loop.runInLoop([&]() {
                counter++;
            });
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < 100; ++i) {
            loop.runInLoop([&]() {
                counter++;
            });
        }
    });

    // Run the event loop in a separate thread
    std::thread loopThread([&]() {
        loop.loop();
    });

    t1.join();
    t2.join();

    // Allow some time for the tasks to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    loop.quit();
    loopThread.join();

    // Verify all tasks were executed
    EXPECT_EQ(counter.load(), 200);
}

TEST(EventLoopTest, QuitBehavior) {
    reactor::EventLoop loop;

    // Run the event loop in a separate thread
    std::thread loopThread([&]() {
        loop.loop();
    });

    // Allow the loop to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Quit the loop
    loop.quit();
    loopThread.join();

    // Verify the loop exited cleanly
    SUCCEED();
}

TEST(EventLoopTest, Reentrancy) {
    reactor::EventLoop loop;
    std::atomic<int> counter = 0;

    // Schedule a task that schedules another task
    loop.runInLoop([&]() {
        counter++;
        loop.runInLoop([&]() {
            counter++;
        });
    });

    // Run the event loop in a separate thread
    std::thread loopThread([&]() {
        loop.loop();
    });

    // Allow some time for the tasks to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    loop.quit();
    loopThread.join();

    // Verify both tasks were executed
    EXPECT_EQ(counter.load(), 2);
}