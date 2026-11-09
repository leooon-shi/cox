#pragma once

#include "timer.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward-declare OpenSSL types to keep the header free of <openssl/*> so
// callers don't need OpenSSL include paths just to hold a context.
struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;
struct ssl_session_st;
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
struct evp_mac_ctx_st;
typedef struct evp_mac_ctx_st EVP_MAC_CTX;

namespace reactor {
class EventLoop;
}

namespace tls {
// Role of a TLS endpoint.
enum class Role { kServer, kClient };

class TlsContext;
class TicketKeyManager;
class SessionCache;
using TlsContextPtr = std::shared_ptr<TlsContext>;

// TlsContext wraps an OpenSSL SSL_CTX: the long-lived, shared configuration
// (certificates, private key, protocol versions, verification mode) from which
// per-connection SSL objects are created. One context is typically shared by
// all connections of a server (or client).
//
// TLS 1.2+ is required; TLS 1.3 is preferred and negotiated automatically.
class TlsContext {
public:
    explicit TlsContext(Role role);
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    // Load a PEM certificate chain and private key (server, or client with a
    // client cert). Throws std::runtime_error on failure. The key must match
    // the certificate.
    void useCertificateAndKey(const std::string& cert_pem_path,
                              const std::string& key_pem_path);

    // Reload the certificate/key (and CA, if previously loaded) from the paths
    // last passed to useCertificateAndKey()/loadCaFile(). Used for hot cert
    // rotation (e.g. Kubernetes Secret updates): new connections created after
    // reload() use the fresh material; existing connections are unaffected.
    // Returns true on success; on failure the previous material remains in
    // effect (atomic-ish: a bad new cert does not break the running context).
    bool reload();

    // Load CA certificates used to verify the peer (a PEM bundle file). Enables
    // peer verification when verify_peer is set.
    void loadCaFile(const std::string& ca_pem_path);

    // Require and verify the peer's certificate. For a server this requests a
    // client certificate (mutual TLS); for a client this verifies the server
    // certificate against the loaded CA(s). Default: client verifies server,
    // server does not request a client cert.
    void setVerifyPeer(bool verify);

    // ---- SNI (Server Name Indication), server role only ----
    //
    // A client sends the hostname it intends to reach in the TLS handshake
    // (RFC 6066), BEFORE any certificate is chosen. That is what makes it
    // possible for one port to serve several domains with different
    // certificates -- virtual hosting, multi-tenant, reverse proxies.
    //
    // Register a per-hostname context with addSniContext(); this context stays
    // the DEFAULT, used when the client sends no SNI or an unknown name (an
    // unknown name deliberately falls back rather than aborting the handshake,
    // matching nginx's default_server behavior).
    //
    // Exact names and single-level wildcards ("*.example.com") are supported;
    // an exact match always wins over a wildcard. Matching is
    // case-insensitive, as DNS names are.
    void addSniContext(const std::string& hostname, TlsContextPtr ctx);

    // Look up the context for a hostname: exact match, then wildcard, else
    // nullptr (meaning "use the default"). Exposed for testing.
    TlsContext* findSniContext(const std::string& hostname) const;

    // Number of registered SNI hostnames.
    size_t sniCount() const;

    // The hostname the peer requested on this connection, or "" if none. Valid
    // once the handshake has progressed past ClientHello.
    static std::string requestedServerName(struct ssl_st* ssl);

    // ---- ALPN (Application-Layer Protocol Negotiation, RFC 7301) ----
    //
    // ALPN lets client and server agree on the application protocol (e.g.
    // "h2" for HTTP/2, "http/1.1") DURING the TLS handshake, with no extra
    // round trip. Set the protocol list on both roles:
    //   * Client: the list is ADVERTISED, in preference order.
    //   * Server: the list is the server's PREFERENCE order; a selection
    //     callback picks the first server-preferred protocol the client also
    //     offered.
    // Protocol identifiers are the ALPN wire names ("h2", "http/1.1"), without
    // length prefixes -- this method encodes the wire format.
    //
    // If the client offers nothing, or client and server share no protocol,
    // the handshake still completes with no ALPN protocol selected
    // (negotiatedProtocol() returns ""); the caller decides how to treat that
    // (typically: assume HTTP/1.1). Call before creating connections.
    void setAlpnProtocols(const std::vector<std::string>& protocols);

    // ---- TLS session resumption ----
    //
    // Resumption skips the full handshake (certificate exchange + asymmetric
    // key agreement) for a RETURNING peer, cutting latency and CPU. See
    // docs/insights/features/tls-session-resumption-design.md.
    //
    // SERVER: enable rotated session TICKETS. The server seals session state
    // into an opaque ticket with a managed key (TicketKeyManager) and resumes
    // statelessly when the client presents it. `loop` drives a periodic
    // ticket-key rotation timer (forward secrecy); `rotate_seconds` is the
    // cadence (0 keeps the default). Must be called on the loop thread, before
    // connections are created. No-op on a client context.
    void enableSessionTickets(reactor::EventLoop* loop,
                              uint32_t rotate_seconds = 0);

    // CLIENT: enable an origin-keyed session cache so a reconnect to the same
    // origin resumes. `max_sessions` bounds the cache (LRU). Must be called
    // before connections are created. No-op on a server context.
    void setSessionCache(size_t max_session = 1024);

    // Disable resumption on this context: no tickets issued/accepted and no
    // client caching -- every handshake is full. Useful where a fresh handshake
    // is required (e.g. re-authentication boundaries).
    void disableResumption();

    // Client-side cache accessor (nullptr if not enabled). Used by TLSEngine to
    // offer/store sessions by origin. Logically const: the cache has its own
    // synchronisation.
    SessionCache* sessionCache() const { return session_cache_.get(); }

    // SSL ex-data index TlsEngine uses to stash the per-connection origin to
    // the new-session callback can key the cache. Static, process-wide.
    static int sessionOriginExIndex();

    Role role() const { return role_; }
    SSL_CTX* nativeHandle() const { return ctx_; }

    // Handshake outcome counters for connections built on this context by
    // TlsConnection (lifetime counts, relaxed atomics, readable from any
    // thread, e.g. a metrics scrape). "Failed" covers both a protocol error and
    // a peer that closed before the handshake completed. Logically const:
    // recording an outcome does not change the TLS configuration.
    uint64_t handshakesOk() const {
        return handshakes_ok_.load(std::memory_order_relaxed);
    }
    uint64_t handshakesFailed() const {
        return handshakes_failed_.load(std::memory_order_relaxed);
    }
    void recordHandshake(bool ok) const {
        (ok ? handshakes_ok_ : handshakes_failed_)
            .fetch_add(1, std::memory_order_relaxed);
    }

private:
    // OpenSSL callback invoked during ClientHello with the requested name.
    static int sniCallback(struct ssl_st* ssl, int* alert, void* arg);
    // OpenSSL server-side ALPN selection callback.
    static int alpnSelectCallback(struct ssl_st* ssl, const unsigned char** out,
                                  unsigned char* outlen,
                                  const unsigned char* in, unsigned int inlen,
                                  void* arg);

    Role role_;
    SSL_CTX* ctx_;
    mutable std::atomic<uint64_t> handshakes_ok_{0};
    mutable std::atomic<uint64_t> handshakes_failed_{0};
    // Remembered paths for reload() (hot cert rotation).
    std::string cert_path_;
    std::string key_path_;
    std::string ca_path_;
    // ALPN protocol list in wire format (length-prefixed), used by the server
    // selection callback and retained so the client advertisement outlives the
    // set call. Empty when ALPN is not configured.
    std::string alpn_wire_;
    // SNI registry: lowercased hostname -> context. Held by shared_ptr because
    // a selected context must outlive the handshake that selected it.
    std::map<std::string, TlsContextPtr> sni_contexts_;

    // ---- Resumption state ----
    // Server: manages + rotates ticket keys; installed via the EVP ticket-key
    // callback. Client: caches sessions by origin. At most one is set per role.
    std::unique_ptr<TicketKeyManager> ticket_keys_;
    std::unique_ptr<SessionCache> session_cache_;
    // Ticket-key rotation timer (server). Owned here; cancelled in dtor.
    reactor::EventLoop* ticket_loop_{nullptr};
    reactor::TimerId ticket_timer_;
    bool ticket_timer_armed_{false};

    // Static trampoline for SSL_CTX_set_tlsext_ticket_key_evp_cb -> the manager
    // reachable via SSL_CTX app-data.
    static int ticketKeyCallback(struct ssl_st* ssl, unsigned char* key_name,
                                 unsigned char* iv, EVP_CIPHER_CTX* ectx,
                                 EVP_MAC_CTX* hctx, int enc);
    // Static trampoline for SSL_CTX_sess_set_new_cb (client): store the new
    // session in the origin-keyed cache. Returns 0 (does not take the
    // callback's reference; SessionCache::store keeps its own dup'd copy).
    static int newSessionCallback(struct ssl_st* ssl,
                                  struct ssl_session_st* session);
    // Lazily-allocated SSL ex-data index holding the per-connection origin
    // C-string (set by TlsEngine), read by newSessionCallback.
    static int originExIndex();
};

}  // namespace tls
