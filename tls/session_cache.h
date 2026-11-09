#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

struct ssl_session_st;
typedef struct ssl_session_st SSL_SESSION;

namespace tls {

// SessionCache is a client-side store of resumable TLS sessions, keyed by
// origin (the hostname/identity the client connected to). Reusing a cached
// session on the next connection to the same origin lets the client offer it in
// the ClientHello and resume -- skipping the certificate exchange and the
// asymmetric key agreement of a full handshake.
//
// Why Cox needs this rather than OpenSSL's built-in client cache: OpenSSL's
// internal client cache is keyed by the session's own id and is not reused
// across independent SSL objects by HOSTNAME, which is the key a connection
// pool actually reconnects on. This cache is keyed by origin so
// http::ConnectionPool / MultiOriginPool (which key connections by origin) get
// resumption for free on a pooled reconnect.
//
// Bounded (LRU): capped at `max_sessions` entries so a client talking to many
// origins cannot grow the cache without limit -- the same drop-and-bound
// discipline used elsewhere in Cox. Thread-safe (a mutex guards the map/LRU);
// SSL_SESSION is reference-counted, so store() ups the refcount and the cache
// drops it on eviction/replacement/clear.
class SessionCache {
public:
    explicit SessionCache(size_t max_sessions);
    ~SessionCache();

    SessionCache(const SessionCache&) = delete;
    SessionCache& operator=(const SessionCache&) = delete;

    // Store (or replace) the session for `origin`. Keeps an INDEPENDENT copy
    // (SSL_SESSION_dup); the caller retains ownership of its own reference.
    // A copy, rather than a shared up-ref, is required because OpenSSL marks a
    // TLS 1.3 session non-resumable once its originating SSL connection is
    // freed -- a pooled reconnect happens after the old connection is gone, so
    // a shared ref would stop offering a PSK. Evicts the least-recently-used
    // entry if at capacity.
    void store(const std::string& origin, SSL_SESSION* session);

    // Return a session to offer for `origin`, or nullptr if none cached. The
    // returned session has an ADDED reference the caller must release with
    // SSL_SESSION_free (SSL_set_session itself does not take ownership). Marks
    // the entry most-recently-used.
    SSL_SESSION* get(const std::string& origin);

    size_t size() const;

private:
    void dropLocked(SSL_SESSION* s);  // SSL_SESSION_free wrapper

    mutable std::mutex mu_;
    const size_t max_;
    // LRU: front = most recently used. The map points at list nodes for O(1)
    // move-to-front and erase.
    std::list<std::pair<std::string, std::string>> lru_;
    std::unordered_map<std::string,
                       std::list<std::pair<std::string, std::string>>::iterator>
        index_;
};

}  // namespace tls
