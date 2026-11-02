#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace reactor {
class EventLoop;
}
namespace concurrent {
class ThreadPool;
}
namespace tls {
class TlsContext;
// Options controlling OcspStapler::startAutoRefresh (namespace-scope so its
// default member initializers are usable as a default argument).
struct OcspRefreshOptions {
    double refresh_ratio = 0.5;       // fraction of remaining lifetime
    int64_t min_seconds = 300;        // never refresh more often than 5m
    int64_t max_seconds = 24 * 3600;  // nor less often than daily
    int64_t fallback_seconds = 3600;  // used when nextUpdate is absent
    int64_t retry_seconds = 300;      // used after a failed/empty fetch
    bool refresh_now = true;          // kick an immediate fetch on start
};

// OCSP stapling (RFC 6961 / RFC 6066 "status_request"): the SERVER fetches a
// signed, time-stamped "this certificate is not revoked" proof from the CA's
// OCSP responder and attaches ("staples") it to the TLS handshake.
//
// WHY: without stapling, a client that wants to check revocation must contact
// the CA itself -- which costs an extra round trip on the critical path, leaks
// which sites the user visits to the CA, and usually fails soft (browsers
// proceed when the responder is unreachable, so revocation checking becomes
// mostly decorative). Stapling moves that fetch to the server, where it happens
// once per refresh period instead of once per client, and the client gets the
// proof for free inside the handshake it was already doing.
//
// WHEN IT MATTERS: only for publicly-trusted, CA-issued certificates served to
// public clients. It is pointless for private-CA, self-signed, or short-lived
// certificates -- there is no responder to ask, and a certificate that expires
// in hours cannot meaningfully be revoked. See insights/roadmap.md.
//
// DESIGN: the response is fetched OUT OF BAND (a background refresh, not during
// a handshake) and cached. A handshake only copies the cached bytes, so TLS
// never blocks on the network. If no valid response is cached, the handshake
// proceeds WITHOUT a staple -- degraded, not broken. This "fail open" choice is
// deliberate: a responder outage must not take the server down, and clients
// already tolerate a missing staple.
class OcspStapler {
public:
    // `cert_path` is the server certificate (leaf); `issuer_path` is the CA
    // that signed it, needed to build the OCSP request. The responder URL is
    // taken from the certificate's AIA extension unless overridden.
    OcspStapler(std::string cert_path, std::string issuer_path);
    ~OcspStapler();

    OcspStapler(const OcspStapler&) = delete;
    OcspStapler& operator=(const OcspStapler&) = delete;

    // Override the responder URL (otherwise read from the cert's AIA
    // extension). Useful for testing against a local responder.
    void setResponderUrl(std::string url) { responder_url_ = std::move(url); }

    // The URL that will be used: the override if set, else the cert's AIA.
    std::string responderUrl() const;

    // Fetch a fresh response from the responder over HTTP and cache it if
    // valid. BLOCKING: call from a worker thread or a timer, never from an
    // event loop. Returns true when a valid response was cached.
    bool refresh();

    // Load a DER-encoded response from a file instead of fetching it. This is
    // the deployment most production setups actually use: an external agent
    // (certbot, a sidecar) writes the response and the server just serves it.
    bool loadFromFile(const std::string& der_path);

    // Raw DER bytes of the cached response, or empty if none is cached.
    std::string cachedResponse() const;
    bool hasResponse() const;

    // Seconds until the cached response's nextUpdate.
    //    > 0  : time remaining before the staple goes stale.
    //      0  : no response cached, or the cached one has expired.
    //     -1  : cached, but the response carries NO nextUpdate field. That is
    //           legal (nextUpdate is OPTIONAL in RFC 6960 §4.2.1) and means the
    //           responder gives no expiry hint, so the caller must pick its own
    //           refresh interval rather than treating it as expired.
    // Use it to schedule the next refresh well before expiry.
    int64_t secondsUntilExpiry() const;

    // Install the stapling callback on a server context, so handshakes serve
    // the cached response. The stapler must outlive the context.
    void attachTo(TlsContext& ctx);

    // --- Automatic refresh -------------------------------------------------
    // Drive refresh() on a schedule so the staple never goes stale in
    // production (the roadmap calls manual refresh a footgun). The blocking
    // network fetch runs on `pool` (never the loop); rescheduling happens on
    //`loop`. The next delay is derived from secondsUntilExpiry():
    //  > 0 : refresh at `refresh_ratio` of the remaining lifetime (default
    //        0.5, i.e. halfway to nextUpdate), clamped to [min, max].
    //   -1 : no nextUpdate hint in the response -> use `fallback_seconds`.
    //    0 : nothing cached / expired / fetch failed -> retry after
    //        `retry_seconds`.
    // Thread-safe to call once after construction. The stapler, loop and pool
    // must all outlive the auto-refresh; call stopAutoRefresh() before any of
    // them is destroyed.
    void startAutoRefresh(reactor::EventLoop& loop,
                          concurrent::ThreadPool& pool,
                          OcspRefreshOptions opts = {});

    // Cancel the scheduled refresh. Safe to call from the loop thread; safe to
    // call if auto-refresh was never started. Must run before loop/pool die.
    void stopAutoRefresh();

    // Validate and cache a DER response (exposed for testing).
    bool setResponseDer(const std::string& der);

private:
    static int statusCallback(struct ssl_st* ssl, void* arg);

    std::string cert_path_;
    std::string issuer_path_;
    std::string responder_url_;

    mutable std::mutex mutex_;
    std::string response_der_;  // Cached DER, guarded by mutex_
    int64_t next_update_epoch_{0};

    // Auto-refresh scheduling state. Defined in the .cc (pimpl) so this header
    // need not pull in the reactor/timer types. Null unless startAutoRefresh
    // was called.
    struct AutoRefresh;
    std::unique_ptr<AutoRefresh> auto_;

    // Internal: compute the next refresh delay (ms) and (re)arm the loop timer.
    void scheduleNextRefresh();
    // Internal: run the blocking fetch on the pool, then reschedule on the
    // loop.
    void doRefreshCycle();
};

}  // namespace tls
