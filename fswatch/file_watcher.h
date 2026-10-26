#pragma once

#include "io_channel.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace reactor
{
class EventLoop;
}

namespace fswatch
{
// FileWatcher monitors filesystem paths for changes using Linux inotify,
// integrated with an EventLoop via a Channel. It is a general-purpose
// primitive (config hot-reload, certificate rotation, data-dir watching) with
// no knowledge of what the changes mean.
//
// Kubernetes note: ConfigMap/Secret volumes are updated by an ATOMIC SYMLINK
// SWAP -- the real files live under a hidden "..data" directory and the mount
// flips a "..data" symlink via rename. A plain IN_MODIFY watch on the target
// file therefore never fires. To catch this, watch the DIRECTORY and react to
// IN_CREATE / IN_MOVED_TO / IN_MOVED_FROM (the ..data rename), which this class
// surfaces via watchDirectory().
//
// Must be created and used on its EventLoop's thread.
class FileWatcher {
public:
    // Invoked (on the loop thread) when a watched path changes. `path` is the
    // watched path that saw activity.
    using Callback = std::function<void(const std::string& path)>;

    explicit FileWatcher(reactor::EventLoop* loop);
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Watch a directory for entry creation/deletion/rename/modification. This
    // is the correct choice for Kubernetes mounted Secrets/ConfigMaps. The
    // callback fires (coalesced) when any relevant event occurs in the dir.
    // Returns true on success.
    bool watchDirectory(const std::string& dir_path, Callback cb);

    // Watch a single file for modification. Note: for atomically-replaced files
    // (K8s, editors that rename-on-save) prefer watchDirectory on the parent.
    bool watchFile(const std::string& file_path, Callback cb);

    // Stop watching a previously watched path.
    void unwatch(const std::string& path);

private:
    void handleRead();

    reactor::EventLoop* loop_;
    int inotify_fd_{-1};
    std::unique_ptr<reactor::Channel> channel_;
    // watch descriptor -> (path, callback)
    struct Watch {
        std::string path;
        Callback callback;
    };
    std::unordered_map<int, Watch> watches_;            // wd -> watch
    std::unordered_map<std::string, int> by_path_;      // path -> wd
};
} // namespace fswatch
