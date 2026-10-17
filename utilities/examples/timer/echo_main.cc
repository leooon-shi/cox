#include "CancellationSource.h"
#include "JitterBackoff.h"
#include "TimerHeap.h"
#include "TimerStatsReporter.h"

#include <iostream>
#include <thread>
#include <chrono>

using namespace std::chrono;

int main() {
    TimerStatsReporter reporter;
    TimerHeap heap(&reporter);
    JitterBackoff backoff(100, 1600); // base: 100ms, cap: 1600ms

    CancellationSource cancelSource = CancellationSource::create();
    CancellationToken token = cancelSource.token();

    // Background polling thread (simulating EventLoop)
    std::thread eventLoop([&]() {
        while (!token.isCancelled()) {
            heap.poll();
            std::this_thread::sleep_for(10ms);
        }
    });

    // Simulate backoff-based retries
    for (int i = 0; i < 5; ++i) {
        uint64_t delay = backoff.next();
        auto execTime = TimerHeap::Clock::now() + milliseconds(delay);

        heap.addTimer(execTime, [i, delay]() {
            std::cout << "Timer " << i << " fired after " << delay << "ms\n";
        }, token);

        std::this_thread::sleep_for(50ms); // simulate short delay between scheduling
    }

    // Simulate cancel trigger after some time
    std::this_thread::sleep_for(800ms);
    cancelSource.cancel();
    eventLoop.join();

    std::cout << "Timers created:   " << reporter.created() << "\n";
    std::cout << "Timers cancelled: " << reporter.cancelled() << "\n";
    std::cout << "Timers expired:   " << reporter.expired() << "\n";

    return 0;
}