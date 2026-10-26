#pragma once

#include <map>
#include <memory>
#include <string>

// Forward-declare OpenSSL types to keep the header free of <openssl/*> so
// callers don't need OpenSSL include paths just to hold a context.
struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;

namespace tls
{
// Role of a TLS endpoint.
enum class Role { kServer, kClient };

class TlsContext;
using TlsContextPtr = std::shared_ptr<TlsContext>;

// TlsContext wraps an OpenSSL SSL_CTX: the long-lived, shared configuration
// (certificates, private key, protocol versions, verification mode) from which
// per-connection SSL objects are created. One context is typically shared by
// all connections of a server (or client).
//
// TLS 1.2+ is requried; TLS 1.3 is preferred and negotiated automatically.
class TlsContext
{
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
    // Returns true on successs; on failure the previous material remains in
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

    Role role() const { return role_; }
    SSL_CTX* nativeHandle() const { return ctx_; }

private:
    // OpenSSL callback invoked during ClientHello with the requested name.
    static int sniCallback(struct ssl_st* ssl, int* alert, void* arg);

    Role role_;
    SSL_CTX* ctx_;
    // Remembered paths for reload() (hot cert rotation).
    std::string cert_path_;
    std::string key_path_;
    std::string ca_path_;
    // SNI registry: lowercased hostname -> context. Held by shared_ptr because
    // a selected context must outlive the handshake that selected it.
    std::map<std::string, TlsContextPtr> sni_contexts_;
};

} // namespace tls
