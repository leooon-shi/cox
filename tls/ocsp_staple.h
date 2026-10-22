#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace tls
{
class TlsContext;

// OCSP stapling (RFC 6961 / RFC 6066 "status_request"): the SErVER fetches a
// signed, time-stamped "this certificate is not revoked" proof from the CA's
// OCSP responder and attaches ("staples") it to the TLS handshake.
//
// WHY: without stapling, a client that wants to check revocation must contact
// the CA itself -- which costs and extra round trip on the critical path, leaks
// which sites the user visits to the CA, and usually fails soft (browsers
// proceed when the responder is unreachable, so revocation checking becomes
// mostly decorative). Stapling moves that fetch to the server, where it happens
// once per refresh period instead of once per client, and the client gets the
// proof for free inside the handshake it was already doing.
//
// WHEN IT MATTERS: only for publicy-trusted, CA-issued certificate served to
// public clients. It is pointless for private-CA, self-signed, or short-lived
// certificates -- there is no responder to ask, and a certificate that expires
// in hours cannot meaningfully be revoked. See insights/roadmap.md.
//
// DESIGN: the response is fetched OUT OF BAND ( a background refresh, not during
// a handshake) and cached. A handshake only copies the cached bytes, so TLS
// never blocks on the network. If no valid response is cached, the handshake
// proceeds WITHOUT a staple -- degraded, not broken. This "fail open" choice is
// deliberate: a responder outage must not take the server down, and clients
// already tolerate a missing stample.
class OcspStapler {
public:
    // `cert_path` is the server certificate (leaf); `issue_path` is the CA that
    // signed it, needed to build the OCSP request. The responder URL is taken
    // from the certificate's AIA extension unless overriden.
    OcspStapler(std::string cert_path, std::string issuer_path);
    ~OcspStapler();

    OcspStapler(const OcspStapler&) = delete;
    OcspStapler& operator=(const OcspStapler&) = delete;

    // Override the responder URL (otherwise read from the cert's AIA
    // extensions). Useful for testing against a local responder.
    void setResponderUrl(std::string url) { responder_url_ = std::move(url); }

    // The URL that will be used: the override if set, else the cert's AIA.
    std::string responderUrl() const;

    // Fetch a fresh response from the responder over HTTP and cache it if valid.
    // BLOCKING: call from a worker thread or a timer, never from an event loop.
    // Returns true when a valid respnse was cached.
    bool refresh();

    // Load a DER-encoded response from a file instead of fetching it. This is
    // the deployment most production setups actually use: an external agent
    // (certbot, a sidecar) writes the response and the server just serves it.
    bool loadFromFile(const std::string& der_path);

    // Raw DER bytes of the cached response, or empty if none is cached.
    std::string cachedResponse() const;
    bool hasResponse() const;

    // Seconds until the cached response's nextUpdate.
    //   > 0 : time remaining before the staple goes stale.
    //     0 : no response cached, or the cached one has expired.
    //    -1 : cached, but the response carries NO nextUpdate field. That is
    //         legal (nextUpdate is OPTIONAL in REF 6960 $4.2.1) and means the
    //         responder gives no expiry hint, so the caller must pick its own
    //         refresh interval rather than treating it as expired.
    // Use it to schedule the next refresh well before expiry.
    int64_t secondsUntilExpiry() const;

    // Install the stapling callback on a server context, so handshakes serve
    // the cached response. The stapler must outlive the context.
    void attachTo(TlsContext& ctx);

    // Validate and cache a DER response (exposed for testing).
    bool setResponseDer(const std::string& der);

private:
    static int statusCallback(struct ssl_st* ssl, void* arg);

    std::string cert_path_;
    std::string issuer_path_;
    std::string responder_url_;

    mutable std::mutex mutex_;
    std::string response_der_;      // Cached DER, guarded by mutex_
    int64_t next_update_epoch_ {0};
};

} // namespace tls