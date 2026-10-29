// runInLoop(): marshal work from other threads onto the event-loop thread
//
// Two producer threads post closures to one EventLoop. Every closure runs on
// the loop thread (the printed thread id is the same), which is how Cox keeps
// all per-connection state single-threaded without locks.
//
// Usage: cross_thread_tasks

#include "event_loop.h"

#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <thread>

int main() {
    reactor::EventLoop loop;  // lives on this (main) thread for the whole run
    const std::map<std::string, std::string> items{{"hello", "world"},
                                                   {"Name", " Leon"},
                                                   {"Gender", " Female"},
                                                   {"Hobbies", " Walking"},
                                                   {"Job", " SW Developer"}};
    std::cout << "loop thread " << std::this_thread::get_id() << std::endl;

    std::thread thread1([&]() {
        for (const auto& [key, value] : items) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            // Capture by value: the closure may run after this iteration ends.
            loop.runInLoop([key, value]() {
                std::cout << key << ":" << value << " from thread 1, run on "
                          << std::this_thread::get_id() << std::endl;
            });
        }
    });

    std::thread thread2([&]() {
        for (int i = 0; i < 2; ++i) {
            for (const auto& [key, value] : items) {
                loop.runInLoop([key, value]() {
                    std::cout << key << ":" << value
                              << " from thread 2, run on "
                              << std::this_thread::get_id() << std::endl;
                });
            }
        }
    });

    // Stop after the producers are done. quit() is thread-safe; the loop
    // object itself is destroyed only after loop() returns on this thread.
    // (The previous version deleted the loop from another thread while it was
    // still running -- a use-after-free.)
    std::thread stopper([&]() {
        thread1.join();
        thread2.join();
        loop.runAfter(100, [&loop]() { loop.quit(); });
    });

    loop.loop();
    stopper.join();
    std::cout << "done" << std::endl;
    return 0;
}
