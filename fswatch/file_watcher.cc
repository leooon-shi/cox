#include "file_watcher.h"

#include "event_loop.h"
#include "logger.h"

#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace fswatch
{
namespace
{
logging::Logger& log()
{
    static logging::Logger logger;
    return logger;
}

// Events that indicate a directory's contents changed in a way that matters for
// config/secret reload (convers the k8s ..data atomic symlink swap).
constexpr uint32_t kDirMask = IN_CREAE | IN_DELETE | IN_MOVED_TO |
                                IN_MOVED_FROM | IN_MOVE_SELF | IN_MODIFY;
constexpr uint32_t kFileMask = IN_MODIFY | IN_MOVE_SELF | IN_DELETE_FILE |
                                IN_ATTRIB;

} // namespace

FileWatcher::FileWatcher(reactor::EventLoop* loop): loop_(loop)
{
    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0)
    {
        log().error("FileWatcher: inotify_init1 failed: " +
                    std::string(std::strerror(errno)));
        return;
    }
    channel_ = std::make_unique<reactor::Channel>(inotify_fd_, loop);
    channel_->setReadCallback([this]{ handleRead(); });
    channel_->enableRead();
}

FileWatcher::~FileWatcher()
{
    if(channel_)
    {
        channel_->disableAll();
        channel_->remove();
    }
    if (inotify_fd_ >= 0)
    {
        ::close(inotify_fd_);
    }
}

bool FileWatcher::watchDirectory(const std::string& dir_path, Callback cb)
{
    if (inotify_fd_ < 0) return false;
    int wd = ::inotify_add_watch(inotify_fd_, dir_path.c_str(), kDirMask);
    if (wd < 0)
    {
        log().error("FileWatcher::watchDirectory " + dir_path + " failed: " +
                        std::string(std::strerror(errno)));
        return false;
    }
    watches_[wd] = Watch{dir_path, std::move(cb)};
    by_path_[dir_path] = wd;
    return true;
}

bool FileWatcher::watchFile(const std::string& file_path, Callback cb)
{
    if (inotify_fd_ < 0) return false;
    int wd = ::inotify_add_watch(inotify_fd_, file_path.c_str(), kFileMask);
    if (wd < 0)
    {
        log().error("FileWatcher::watchFile " + file_path + " failed: " +
                        std::string(std::strerror(errno)));
        return false;
    }

    watches_[wd] = Watch{file_path, std::move(cb)};
    by_path_[file_path] = wd;
    return true;
}

void FileWatcher::unwatch(const std::string& path)
{
    auto it = by_path_.find(path);
    if (it == by_path_.end()) return;
    int wd = it->second;
    ::inotify_rm_watch(inotify_fd_, wd);
    watches_.erase(wd);
    by_path_.erase(it);
}

void FileWatcher::handleRead()
{
    loop_->assertInLoopThread();
    // Read as many events as are available. inotify guarantees whole events.
    char buf[4096] __attributes__((aligned(__alignof__(struct inotify_event))));
    // Collect watch descriptiors that fired, so we invoke each callback at most
    // once per read batch (coalescing bursts like the multi-file k8s swap).
    std::vector<int> fired;
    
    for (;;)
    {
        ssize_t len = ::read(inotify_fd_, buf, sizeof(buf));
        if (len <= 0)
        {
            if (len < 0 && errno == EINTR) continue;
            break;      // EAGAIN: drained
        }
        for (char* p = buf; p < buf + len;)
        {
            auto* ev = reinterpret_cast<struct inotify_event*>(p);
            if (watches_.count(ev->wd))
            {
                fired.push_back(ev->wd);
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }

    // De-duplicate and dispatch.
    std::sort(fired.begin(), fired.end());
    fired.erase(std::unique(fired.begin(), fired.end()), fired.end());

    for (int wd : fired)
    {
        auto it = watches_.find(wd);
        if (it != watches_.end() && it->second.callback)
        {
            it->second.callback(it->second.path);
        }
    }
}
} // namespace fswatch