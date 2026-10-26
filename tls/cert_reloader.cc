#include "cert_reloader.h"

#include "event_loop.h"
#include "file_watcher.h"
#include "logger.h"

#include <sys/stat.h>

#include <string>
#include <utility>

namespace tls
{
namespace
{
logging::Logger& log()
{
    static logging::Logger logger;
    return logger;
}

long mtimeOf(const std::string& path)
{
    struct stat st;
    if (path.empty() || ::stat(path.c_str(), &st) != 0)
    {
        return 0;
    }
    return static_cast<long>(st.st_mtime);
}
}   // namespce

CertReloader::CertReloader(reactor::EventLoop* loop, TlsContextPtr ctx)
    : loop_(loop), ctx_(std::move(ctx)) {}

CertReloader::~CertReloader() = default;

bool CertReloader::startWatching(const std::string& dir_to_watch)
{
    loop_->assertInLoopThread();
    watcher_ = std::make_unique<fswatch::FileWatcher>(loop_);
    bool ok = watcher_->watchDirectory(dir_to_watch, [this](const std::string&) {
        // Any change in the cert directory triggers a reload attempt. The
        // TlsContext::reload() validates the new material and keeps the old on
        // failure, so a transient/partial state is safe.
        reloadNow();
    });
    if (!ok)
    {
        log().error("CertReloader: failed to watch " + dir_to_watch);
        watcher_.reset();
    }
    else
    {
        log().info("CertReloader: watching " + dir_to_watch +
                   " for cert rotation");
    }
    return ok;
}

void CertReloader::startPolling(const std::string& cert_path,
                                const std::string& key_path,
                                const std::string& ca_path,
                                uint64_t interval_ms)
{
    loop_->assertInLoopThread();
    cert_path_ = cert_path;
    key_path_ = key_path;
    ca_path_ = ca_path;
    cert_mtime_ = mtimeOf(cert_path_);
    key_mtime_ = mtimeOf(key_path_);
    ca_mtime_ = mtimeOf(ca_path_);
    polling_ = true;

    loop_->runEvery(interval_ms, [this]() {
        if (polling_ && filesChanged())
        {
            log().info("CertReloader: cert files changed (poll); reloading");
            reloadNow();
        }
    });
    log().info("CertReloader: polling cert files every " +
               std::to_string(interval_ms) + " ms");
}

bool CertReloader::filesChanged()
{
    long c = mtimeOf(cert_path_);
    long k = mtimeOf(key_path_);
    long a = mtimeOf(ca_path_);
    bool changed = (c != cert_mtime_) || (k != key_mtime_) || (a != ca_mtime_);
    if (changed)
    {
        cert_mtime_ = c;
        key_mtime_ = k;
        ca_mtime_ = a;
    }
    return changed;
}

void CertReloader::reloadNow()
{
    loop_->assertInLoopThread();
    bool ok = ctx_->reload();
    if (ok)
    {
        log().info("CertReloader: TLS certificate reloaded");
    }
    else
    {
        log().error("CertReloader: TLS certificate reload failed; "
                    "keeping previous certificate");
    }
    if (reload_callback_)
    {
        reload_callback_(ok);
    }
}

}   // namespace tls
