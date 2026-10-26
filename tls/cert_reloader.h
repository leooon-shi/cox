#pragma once

#include "tls_context.h"

#include <functional>
#include <memory>
#include <string>

namespace reactor
{
class EventLoop;
}

namespace fswatch
{
class FileWatcher;
}

namespace tls
{
// CertReloader keeps a TlsContext's certificate material fresh as it rotates on
// disk (e.g. Kubernetes Secret updates, cert-manager, SPIFFE). It triggers
// TlsContext::reload() when the mounted cert files chagne, so new connections
// pick up the new certificate without a process restart. Existing connections
// keep their negotiated session.
//
// Two trigger modes:
//   - Watch (inotify): reacts immediately, and correctly handles the k8s
//     atomic-symlink-swap by watching the mount DIRECTORY.
//   - Poll (timer): checks file modification times every interval; simpler,
//     no inotify, good when a small reload latency is acceptable.
//
// Must be created and used on its EventLoop's thread.
class CertReloader {
public:
    // Fired (on the loop thread) after a reload attempt. `ok` is the result of
    // TlsContext::reload(). Optional; useful for logging/metrics.
    using ReloadCallback = std::function<void(bool ok)>;
    
    // dir_to_watch: the directory containing the cert files (for K8s, the
    // Secret mount dir). Watching the directory catches the atomic swap.
    CertReloader(reactor::EventLoop* loop, TlsContextPtr ctx);
    ~CertReloader();

    CertReloader(const CertReloader&) = delete;
    CertReloader& operator=(const CertReloader&) = delete;

    void setReloadCallback(ReloadCallback cb) { reload_callback_ = std::move(cb); }

    // Start inotify-based reloading: watch `dir_to_watch` and reload on change.
    // Returns true if the watch was established.
    bool startWatching(const std::string& dir_to_watch);

    // Start timer-based reloading: poll the cert/key/CA files every
    // internal_ms and reload if any modification time changed.
    void startPolling(const std::string& cert_path, const std::string& key_path,
                      const std::string& ca_path, uint64_t interval_ms);
    // Trigger a reload now (also invoked internally by the triggers).
    void reloadNow();

private:
    bool filesChanged();  // for poll mode: compare mtimes

    reactor::EventLoop* loop_;
    TlsContextPtr ctx_;
    std::unique_ptr<fswatch::FileWatcher> watcher_;
    ReloadCallback reload_callback_;

    // Poll-mode state.
    std::string cert_path_, key_path_, ca_path_;
    long cert_mtime_{0}, key_mtime_{0}, ca_mtime_{0};
    bool polling_{false};
};

}   // namespace tls
