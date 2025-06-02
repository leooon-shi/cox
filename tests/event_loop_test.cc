#include "event_loop.h"
#include "poller/io_poller.h"
#include "io_channel.h"

#include <gtest/gtest.h>
#include "gmock/gmock.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <future>

class MockPoller : public reactor::Poller {
public:
    MOCK_METHOD(void, updateChannel, (reactor::Channel* channel), (override));
    MOCK_METHOD(void, removeChannel, (reactor::Channel* channel), (override));
    MOCK_METHOD(void, poll, (int timeoutMs, std::vector<reactor::Channel*>& activeChannels), (override));
};

#if defined(DEBUG)
TEST(EventLoopTest, AddAndRemoveChannel) {
    std::unique_ptr<MockPoller> pollerPtr = std::make_unique<MockPoller>();
    // Set the Poller for the EventLoop
    reactor::EventLoop loop(std::move(pollerPtr));
    MockPoller& poller = static_cast<MockPoller&>(loop.getPoller());

    // Create a Channel object with the required constructor arguments
    int fd = 1; // Example file descriptor
    {
        // Using block to avoid EventLoop::~EventLoop() being called before the test completes
        reactor::Channel channel(fd, &loop);

        // Expect the Poller to update the channel when added
        EXPECT_CALL(poller, updateChannel(&channel)).Times(1);

        // Add the channel to the EventLoop
        loop.updateChannel(&channel);

        // Expect the Poller to remove the channel when removed
        EXPECT_CALL(poller, removeChannel(&channel)).Times(1);

        // Remove the channel from the EventLoop
        loop.removeChannel(&channel);
    }
    // Ensure the EventLoop is still valid after the channel is removed
    // Expect the poller to handle wakeup channel during EventLoop destruction
    EXPECT_CALL(poller, updateChannel(testing::_)).Times(1);
    EXPECT_CALL(poller, removeChannel(testing::_)).Times(1);

    // Quit the loop to trigger cleanup
    loop.quit();
}
#endif // DEBUG

TEST(EventLoopTest, HandlePendingTasks) {
    reactor::EventLoop loop;
    bool taskExecuted = false;

    // Schedule a task to run in the loop
    loop.runInLoop([&]() {
        taskExecuted = true;
    });

    // Run the loop to process pending tasks
    std::thread loopThread([&]() {
        // Allow some time for the task to execute
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        loop.quit();
    });

    loop.loop();
    loopThread.join();
    // Verify that the task was executed
    ASSERT_TRUE(taskExecuted);
}

#if defined(DEBUG)
TEST(EventLoopTest, PollerHandlesEvents) {
    std::unique_ptr<MockPoller> poller_p = std::make_unique<MockPoller>();
    MockPoller& poller = *poller_p;

    int fd = 1;
    
    reactor::EventLoop* loopPtr = nullptr;

    // Create a Channel object with the required constructor arguments
    reactor::Channel channel(fd, nullptr);

    std::vector<reactor::Channel*> activeChannels = {&channel};

    // Expect the Poller to poll for events and return active channels
    EXPECT_CALL(poller, poll(testing::_, testing::_))
        .WillOnce(::testing::Invoke([&](int timeoutMs, std::vector<reactor::Channel*>& channels) {
            channels = activeChannels;
            loopPtr->quit();
        }));

    // Run the loop to process events
    std::thread loopThread([&]() {
        reactor::EventLoop loop(std::move(poller_p));
        loopPtr = &loop;
        loop.loop();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    loopThread.join();
}
#endif // DEBUG

TEST(EventLoopTest, TaskScheduling) {
    reactor::EventLoop loop;
    std::atomic<int> counter = {0};

    // Schedule a task
    loop.runInLoop([&]() {
        counter++;
    });

    // Run the event loop in a separate thread
    std::thread loopThread([&]() {
        // Allow some time for the task to execute
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        loop.quit();
    });
    
    loop.loop();
    loopThread.join();

    // Verify the task was executed
    EXPECT_EQ(counter.load(), 1);
}

TEST(EventLoopTest, Concurrency) {
    reactor::EventLoop loop;
    std::atomic<int> counter = {0};

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
        // Allow some time for the tasks to execute
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        loop.quit();
    });


    t1.join();
    t2.join();
    
    loop.loop();

    loopThread.join();

    // Verify all tasks were executed
    EXPECT_EQ(counter.load(), 200);
}

TEST(EventLoopTest, QuitBehavior) {
    reactor::EventLoop loop;

    // Run the event loop in a separate thread
    std::thread loopThread([&]() {
        // Allow the loop to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
        // Quit the loop
        loop.quit();
    });

    loop.loop();
    loopThread.join();

    // Verify the loop exited cleanly
    SUCCEED();
}

TEST(EventLoopTest, Reentrancy) {
    reactor::EventLoop loop;
    std::atomic<int> counter = {0};

    // Schedule a task that schedules another task
    loop.runInLoop([&]() {
        counter++;
        loop.runInLoop([&]() {
            counter++;
        });
    });

    // Run the event loop in a separate thread
    std::thread loopThread([&]() {
        // Allow some time for the tasks to execute
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        loop.quit();
    });

    loop.loop();
    loopThread.join();

    // Verify both tasks were executed
    EXPECT_EQ(counter.load(), 2);
}