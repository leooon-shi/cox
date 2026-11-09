#include "tls_context.h"

#include "event_loop.h"
#include "session_cache.h"
#include "ticket_key_manager.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <mutex>
#include <stdexcept>
#include <string>

namespace tls {
namespace {

std::string opensslError(const std::string& what) {
    unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return what + ": " + buf;
}

// One-time process-wide OpenSSL init. OpenSSL 1.1+/3.0 auto-initializes, but we
// make it explicit and idempotent.
void ensureOpenSslInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    });
}
}  // namespace

TlsContext::TlsContext(Role role) : role_(role), ctx_(nullptr) {
    ensureOpenSslInit();

    const SSL_METHOD* method =
        (role == Role::kServer) ? TLS_server_method() : TLS_client_method();
    ctx_ = SSL_CTX_new(method);
    if (ctx_ == nullptr) {
        throw std::runtime_error(opensslError("SSL_CTX_new failed"));
    }

    // Require TLS 1.2 minimum; allow TLS 1.3 (negotiated automatically).
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

    // Sensible defaults: no SSLv2/v3, honor server cipher order, enable
    // automatic retry so SSL_read/SSL_write don't return on renegotiation.
    SSL_CTX_set_options(
        ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx_, SSL_MODE_AUTO_RETRY |
                               SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                               SSL_MODE_ENABLE_PARTIAL_WRITE);

    // Client verifies the server by default; server does not request a client
    // cert unless setVerifyPeer(true) is called.
    if (role == Role::kClient) {
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ctx_);  // system CA store
    } else {
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    }
}

TlsContext::~TlsContext() {
    // Cancel the ticket-key rotation timer before anything it closes over is
    // destroyed (the sanitizer-saga discipline: cancel armed timers on
    // teardown). ticket_keys_/session_cache_ then free via unique_ptr.
    if (ticket_timer_armed_ && ticket_loop_ != nullptr) {
        ticket_loop_->cancel(ticket_timer_);
        ticket_timer_armed_ = false;
    }
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

int TlsContext::ticketKeyCallback(SSL* ssl, unsigned char* key_name,
                                  unsigned char* iv, EVP_CIPHER_CTX* ectx,
                                  EVP_MAC_CTX* hctx, int enc) {
    // Recover the manager from the SSL_CTX app-data set in
    // enableSessionTickets.
    SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
    auto* mgr = static_cast<TicketKeyManager*>(SSL_CTX_get_ex_data(ctx, 0));
    if (mgr == nullptr) {
        return -1;
    }
    return mgr->keyCallback(ssl, key_name, iv, ectx, hctx, enc);
}

void TlsContext::enableSessionTickets(reactor::EventLoop* loop,
                                      uint32_t rotate_seconds) {
    if (role != Role::kServer) {
        return;  // tickets are a server concern
    }
    if (!ticket_keys_) {
        ticket_keys_ = std::make_unique<TicketKeyManager>();
    }
    // Make the manager reachable from the C callback via SSL_CTX app-data.
    SSL_CTX_set_ex_data(ctx_, 0, ticket_keys_.get());
    SSL_CTX_set_tlsext_ticket_key_evp_cb(ctx_, &TlsContext::ticketKeyCallback);

    // Periodic rotation for forward secrecy, driven on the loop (same pattern
    // as CertReloader). Defualt cadence is a few hours; keep the previous key
    // for the grace window that TicketKeyManager retains.
    const uint64_t period_ms =
        static_cast<uint64_t>(rotate_seconds == 0 ? 6 * 3600 : rotate_seconds) *
        1000;
    ticket_loop_ = loop;
    if (loop != nullptr) {
        TicketKeyManager* mgr = ticket_keys_.get();
        ticket_timer_ = loop->runEvery(period_ms, [mgr]() { mgr->rotate(); });
        ticket_timer_armed_ = true;
    }
}

void TlsContext::setSessionCache(size max_sessions) {
    if (role != Role::kClient) {
        return;  // the origin-keyed cache is a client concern
    }
    session_cache_ = std::make_unique<SessionCache>(max_sessions);
    // Reach the cache from the C callback via SSL_CTX app-data slot 1 (slot 0
    // is the server ticket manager; a context is single-role so they never
    // collide, but distinct slots keep it clear).
    SSL_CTX_set_ex_data(ctx_, 1, session_cache_.get());
    // Enable client-side caching so OpenSSL surfaces new sessions through the
    // new-session callback; we store them by origin. NO_INTERNAL_STORE: we keep
    // the sessions ourselves, keyed by origin, not in OpenSSL's id-keyed store.
    SSL_CTX_set_session_cache_mode(
        ctx_, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_sess_set_new_cb(ctx_, &TlsContext::newSessionCallback);
}

int TlsContext::newSessionCallback(SSL* ssl, SSL_SESSION* session) {
    // Fires when a session becomes resumble -- for TLS 1.2 at handshake, for
    // TLS 1.3 when the post-handshake NewSessionTicket arrives. Recover the
    // cache (SSL_CTX ex-data) and the origin (SSL ex-data, set by TlsEngine).
    SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
    auto* cache = static_cast<SessionCache*>(SSL_CTX_get_ex_data(ctx, 1));
    const char* origin =
        static_cast<const char*>(SSL_get_ex_data(ssl, originExIndex()));
    if (cache != nullptr && origin != nullptr && origin[0] != '\0') {
        cache->store(origin, session);  // store() dups into an independent copy
    }
    // Return 0: we did NOT take ownership of the callback's reference (store()
    // keeps its own copy), so OpenSSL frees the session as usual.
    return 0;
}

int TlsContext::originExIndex() {
    // Process-wide SSL ex-data index for the per-connection origin C-string.
    static int idx =
        SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return idx;
}

int TlsContext::sessionOriginExIndex() {
    return originExIndex();
}

void TlsContext::disableResumption() {
    // No tickets issued/accepted (server) and no caching (client): full
    // handshake every time.
    SSL_CTX_set_options(ctx_, SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_OFF);
    if (ticket_timer_armed_ && ticket_loop_ != nullptr) {
        ticket_loop_->cancel(ticket_timer_);
        ticket_timer_armed_ = false;
    }
    ticket_keys_.reset();
    session_cache_.reset();
}

void TlsContext::useCertificateAndKey(const std::string& cert_pem_path,
                                      const std::string& key_pem_path) {
    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_pem_path.c_str()) != 1) {
        throw std::runtime_error(
            opensslError("load certificate " + cert_pem_path));
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, key_pem_path.c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
        throw std::runtime_error(
            opensslError("load private key " + key_pem_path));
    }
    if (SSL_CTX_check_private_key(ctx_) != 1) {
        throw std::runtime_error(
            opensslError("private key does not match certificate"));
    }

    cert_path_ = cert_pem_path;
    key_path_ = key_pem_path;
}

void TlsContext::loadCaFile(const std::string& ca_pem_path) {
    if (SSL_CTX_load_verify_locations(ctx_, ca_pem_path.c_str(), nullptr) !=
        1) {
        throw std::runtime_error(opensslError("load CA file " + ca_pem_path));
    }
    ca_path_ = ca_pem_path;
}

bool TlsContext::reload() {
    if (cert_path_.empty() || key_path_.empty()) {
        return false;  // nothing was loaded to reload.
    }
    // Validate the new material into the live SSL_CTX. OpenSSL replaces the
    // current certificate/key on success; on failure the previous ones remain,
    // so a mid-rotation partially-written file cannot break the running server.
    // (Both files are validated together via check_private_key before we
    // consider the reload successful.)
    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_path_.c_str()) != 1) {
        opensslError("reload certificate");  // clear the error queue
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, key_path_.c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
        opensslError("reload private key");
        return false;
    }
    if (SSL_CTX_check_private_key(ctx_) != 1) {
        opensslError("reloaded key/cert mismatch");
        return false;
    }
    if (!ca_path_.empty()) {
        if (SSL_CTX_load_verify_locations(ctx_, ca_path_.c_str(), nullptr) !=
            1) {
            opensslError("reload CA");
            return false;
        }
    }
    return true;
}

void TlsContext::setVerifyPeer(bool verify) {
    if (verify) {
        int mode = SSL_VERIFY_PEER;
        if (role_ == Role::kServer) {
            // Require the client to present a certificate (mutual TLS).
            mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
        SSL_CTX_set_verify(ctx_, mode, nullptr);
    } else {
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    }
}

// ---- SNI ----
namespace {
std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}
}  // namespace

void TlsContext::addSniContext(const std::string& hostname, TlsContextPtr ctx) {
    if (role_ != Role::kServer) {
        throw std::runtime_error("addSniContext is server-role only");
    }
    if (!ctx) {
        throw std::runtime_error("addSniContext: null context");
    }
    if (ctx->role() != Role::kServer) {
        throw std::runtime_error("addSniContext: context must be server role");
    }

    sni_contexts_[toLowerAscii(hostname)] = std::move(ctx);

    // Install the callback lazily, on the first registration, so a server that
    // never uses SNI pays nothing.
    // The OpenSSL macro expands to a C-style function-pointer cast internally,
    // which trips -Wold-style-cast under GCC; suppress it at this call site
    // rather than weakening the flag for our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    SSL_CTX_set_tlsext_servername_callback(ctx_, &TlsContext::sniCallback);
#pragma GCC diagnostic pop
    SSL_CTX_set_tlsext_servername_arg(ctx_, this);
}

TlsContext* TlsContext::findSniContext(const std::string& hostname) const {
    const std::string host = toLowerAscii(hostname);

    // Exact match wins over any wildcard.
    auto it = sni_contexts_.find(host);
    if (it != sni_contexts_.end()) {
        return it->second.get();
    }

    // Single-level wildcard: "*.example.com" matches "a.example.com" but NOT
    // "example.com" nor "a.b.example.com" (RFC 6125 semantics).
    size_t dot = host.find('.');
    if (dot != std::string::npos && dot + 1 < host.size()) {
        std::string wildcard = "*" + host.substr(dot);
        auto w = sni_contexts_.find(wildcard);
        if (w != sni_contexts_.end()) {
            return w->second.get();
        }
    }
    return nullptr;  // caller keeps the default context
}

size_t TlsContext::sniCount() const {
    return sni_contexts_.size();
}

std::string TlsContext::requestedServerName(struct ssl_st* ssl) {
    if (ssl == nullptr) {
        return std::string();
    }
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    return name ? std::string(name) : std::string();
}

int TlsContext::sniCallback(struct ssl_st* ssl, int* /*alert*/, void* arg) {
    auto* self = static_cast<TlsContext*>(arg);
    if (self == nullptr || ssl == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }

    const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (servername == nullptr) {
        // No SNI sent (or an IP literal was used): keep the default context.
        return SSL_TLSEXT_ERR_OK;
    }

    TlsContext* selected = self->findSniContext(servername);
    if (selected == nullptr) {
        // Unknown name: fall back to the default certificate rather than
        // aborting. Failing the handshake here would make an unrecognized Host
        // a hard connection error; nginx's default_server behaves this way too.
        return SSL_TLSEXT_ERR_OK;
    }

    // Swap this connection onto the matched context, so the certificate,
    // key, and verification settings all come from it.
    if (SSL_set_SSL_CTX(ssl, selected->nativeHandle()) == nullptr) {
        return SSL_TLSEXT_ERR_OK;  // keep the default on failure
    }
    // SSL_set_SSL_CTX does NOT copy these, so re-apply them from the chosen
    // context; otherwise a per-host verification setting would be ignored.
    SSL_set_options(ssl, SSL_CTX_get_options(selected->nativeHandle()));
    SSL_set_verify(ssl, SSL_CTX_get_verify_mode(selected->nativeHandle()),
                   SSL_CTX_get_verify_callback(selected->nativeHandle()));

    return SSL_TLSEXT_ERR_OK;
}

void TlsContext::setAlpnProtocols(const std::vector<std::string>& protocols) {
    // Encode to ALPN wire format: each entry is a 1-byte length followed by the
    // protocol name (RFC 7301). e.g. {"h2","http/1.1"} ->
    // \x02 h 2 \x08 h t t p / 1 . 1
    alpn_wire_.clear();
    for (const std::string& p : protocols) {
        if (p.empty() || p.size() > 255) {
            throw std::runtime_error(
                "ALPN protocol name must be 1..255 bytes: '" + p + "'");
        }
        alpn_wire_.push_back(static_cast<char>(p.size()));
        alpn_wire_.append(p);
    }

    if (role_ == Role::kClient) {
        // Client advertises the list directly. OpenSSL copies the buffer, so it
        // need not outlive the call -- but we keep alpn_wire_ anyway for parity
        // and possible reload.
        SSL_CTX_set_alpn_protos(
            ctx_, reinterpret_cast<const unsigned char*>(alpn_wire_.data()),
            static_cast<unsigned int>(alpn_wire_.size()));
    } else {
        // Server installs a selection callback; `this` carries the preference
        // list (alpn_wire_) into the callback.
        SSL_CTX_set_alpn_select_cb(ctx_, &TlsContext::alpnSelectCallback, this);
    }
}

int TlsContext::alpnSelectCallback(struct ssl_st* /*ssl*/,
                                   const unsigned char** out,
                                   unsigned char* outlen,
                                   const unsigned char* in, unsigned int inlen,
                                   void* arg) {
    auto* self = static_cast<TlsContext*>(arg);
    if (self == nullptr || self->alpn_wire_.empty()) {
        // No server preference configured: decline, handshake proceeds with no
        // ALPN protocol selected.
        return SSL_TLSEXT_ERR_NOACK;
    }

    // SSL_select_next_proto picks the first of the SERVER list that also
    // appears in the CLIENT list (server preference wins, matching nghttp2 /
    // nginx behavior). Both buffers are ALPN wire format.
    unsigned char* selected = nullptr;
    unsigned char selected_len = 0;
    const auto* server =
        reinterpret_cast<const unsigned char*>(self->alpn_wire_.data());
    const auto server_len = static_cast<unsigned int>(self->alpn_wire_.size());

    int rc = SSL_select_next_proto(&selected, &selected_len, server, server_len,
                                   in, inlen);
    if (rc != OPENSSL_NPN_NEGOTIATED) {
        // No overlap: do not fail the handshake, just select nothing (the peer
        // falls back, typically to HTTP/1.1).
        return SSL_TLSEXT_ERR_NOACK;
    }
    *out = selected;
    *outlen = selected_len;
    return SSL_TLSEXT_ERR_OK;
}

}  // namespace tls
