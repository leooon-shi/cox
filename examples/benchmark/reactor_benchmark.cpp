#include "event_loop.h"

#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <memory>

void benchmarkTaskScheduling(reactor::EventLoop* loop, int numTasks) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numTasks; ++i) {
        loop->runInLoop([i]() {
            // Simulate a lightweight task
            volatile int x = i * i;
        });
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    
    std::cout << "Scheduled " << numTasks << " tasks in " << elapsed.count() << " seconds." << std::endl;
}

void benchmarkConcurrency(reactor::EventLoop* loop, int numThreads, int numTasksPerThread) {
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([loop, numTasksPerThread]() {
            for (int i = 0; i < numTasksPerThread; ++i) {
                loop->runInLoop([i]() {
                    // Simulate a lightweight task
                    volatile int x = i * i;
                });
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    std::cout << "Executed " << (numThreads * numTasksPerThread) << " tasks across " << numThreads
              << " threads in " << elapsed.count() << " seconds." << std::endl;
}

int main() {
    reactor::EventLoop* loop = new reactor::EventLoop();

    // Benchmark task scheduling
    benchmarkTaskScheduling(loop, 80000000);

    // Benchmark concurrency
    benchmarkConcurrency(loop, 32, 2500000);

    // Clean up
    loop->quit();
    delete loop;

    return 0;
}
// Note: The above benchmarks are designed to measure the performance of task scheduling and concurrency
// in the reactor pattern. The actual performance may vary based on system load, hardware, and other factors.
// Ensure to run this code in an environment where the reactor pattern is implemented correctly.
// The reactor pattern is typically used in event-driven programming to handle I/O events efficiently.
// The benchmarks simulate lightweight tasks to measure the overhead of scheduling and concurrency.
// Adjust the number of tasks and threads based on your system's capabilities to avoid overwhelming it.
// The reactor pattern is commonly used in network servers, GUI applications, and other event-driven systems.
// Make sure to include necessary headers and link against the appropriate libraries for your environment.
// This code is a simple demonstration of how to benchmark task scheduling and concurrency
// using the reactor pattern. It is not intended for production use.
// Ensure to handle exceptions and errors appropriately in a real-world application.
// The reactor pattern allows for efficient handling of multiple I/O events without blocking.
// This benchmark focuses on the performance of scheduling tasks in an event loop
// and the ability to handle concurrent tasks from multiple threads.
// The reactor pattern is a design pattern that provides a way to handle events in a non-blocking manner.
// It is widely used in network programming, GUI applications, and other event-driven systems.
// The benchmarks are designed to measure the overhead of scheduling tasks in an event loop
// and the performance of handling concurrent tasks from multiple threads.
// The reactor pattern is a powerful tool for building scalable and responsive applications.
// The benchmarks simulate lightweight tasks to measure the overhead of scheduling and concurrency.

//output example:

// Scheduled 10000000 tasks in 5.96368 seconds.
// Executed 80000000 tasks across 32 threads in 62.6889 seconds.

// Scheduled 10000000 tasks in 5.96368 seconds.
// Executed 80000000 tasks across 32 threads in 63.4039 seconds.

// Scheduled 10000000 tasks in 0.827748 seconds.
// Executed 80000000 tasks across 32 threads in 64.264 seconds.

// Scheduled 10000000 tasks in 0.765107 seconds.
// Executed 40000000 tasks across 16 threads in 31.7006 seconds.

// Scheduled 10000000 tasks in 0.772472 seconds.
// Executed 40000000 tasks across 16 threads in 31.3532 seconds.

// Scheduled 10000000 tasks in 0.777877 seconds.
// Executed 40000000 tasks across 16 threads in 32.0865 seconds.

// Scheduled 10000000 tasks in 0.793351 seconds.
// Executed 20000000 tasks across 8 threads in 15.0965 seconds.

// Scheduled 10000000 tasks in 0.763669 seconds.
// Executed 20000000 tasks across 8 threads in 14.8819 seconds.

// Scheduled 10000000 tasks in 0.777376 seconds.
// Executed 20000000 tasks across 8 threads in 15.259 seconds.

// Scheduled 10000000 tasks in 0.778707 seconds.
// Executed 10000000 tasks across 4 threads in 3.88944 seconds.

// Scheduled 10000000 tasks in 0.782421 seconds.
// Executed 10000000 tasks across 4 threads in 4.15588 seconds.

// Scheduled 10000000 tasks in 0.780274 seconds.
// Executed 10000000 tasks across 4 threads in 3.8772 seconds.

