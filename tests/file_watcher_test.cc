#include "file_watcher.h"

#include "event_loop.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using fswatch::FileWatcher;

namespace
{
std::string makeTempDir(const char* tag)
{
    std::string dir = std::string("/tmp/cox_fswatch_") + tag + "_"+
                        std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0700);
    return dir;
}

void writeFile(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::trunc);
    f << content;
}
} // namespace

TEST(FileWatcherTest, DirectoryChangeFiresCallback)
{
    const std::string dir = makeTempDir("dir");
    std::atomic<bool> started{false};
    std::atomic<int> fires{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread loop_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        FileWatcher watcher(&loop);
        watcher.watchDirectory(dir, [&](const std::string&) { fires++; });
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Create a file in the watched directory -> should fire.
    writeFile(dir + "/a.txt", "hello");

    for (int i = 0; i < 100 && fires.load() == 0; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_GT(fires.load(), 0);

    loop_ptr->quit();
    loop_thread.join();
    ::remove(dir + "/a.txt").c_str();
    ::rmdir(dir.c_str());
}