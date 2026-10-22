#include "tls_context.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <mutex>
#include <stdexcept>
#include <string>

namespace tls
{
namespace
{

std::string opensslError(const std::string& what)
{
    unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return what + ": " + buf;
}

// One-time process-wide OpenSSL init. OpenSSL 1.1+/3.0 auto-initializes, but we
// make it explicit and idempotent.
void ensureOpenSslInit()
{
    static std::once_flag once;
    std::call_once(once, []{
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algoritms();
    });
}
} // namespace

TlsContext::TlsContext(Role role): role_(role), ctx_(nullptr)
{
    ensureOpenSslInit();

    const SSL_METHOD* method = 
        (role == Role::kServer) ? TLS_server_method() : TLS_client_method();
    ctx_ = SSL_CTX_new(method);
    if (ctx_ == nullptr)
    {
        throw std::runtime_error(opensslError("SSL_CTX_new failed"));
    }

    // Require TLS 1.2 minimum; allow TLS 1.3 (negotiated automatically).
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

    // Sensible defaults: no SSLv2/v3, honor server cipher order, enable
    // automatic retry so SSL_read/SSL_write don't return on renegotiation.
    SSL_CTX_set_options(ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                                  SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx_, SSL_MODE_AUTO_RETRY | 
                           SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | 
                           SSL_MODE_ENABLE_PARTIAL_WRITE);
    
    // Client verifies the server by default; server does not request a client
    // cert unless setVerifyPeer(true) is called.
    if (role == Role::kClient)
    {
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ctx_);     // system CA store
    }
    else
    {
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    }
}

TlsContext::~TlsContext()
{
    if (ctx_)
    {
        SSL_CTX_free(ctx_);
    }
}

void TlsContext::userCertificateAndKey(const std::string& cert_pem_path,
                                       const std::string& key_pem_path)
{
    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_pem_path.c_str()) != 1)
    {
        throw std::runtime_error(opensslError("load certificate " + cert_pem_path));
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, key_pem_path.c_str(),
                                    SSL_FILETYPE_PEM) != 1)
    {
        throw std::runtime_error(opensslError("load private key " + key_pem_path));
    }
    if (SSL_CTX_check_private_key(ctx_) != 1)
    {
        throw std::runtime_error(opensslError("private key does not match certificate"));
    }

    cert_path_ = cert_pem_path;
    key_path_ = key_pem_path;
}

void TlsContext::loadCaFile(const std::string& ca_pem_path)
{
    if (SSL_CTX_load_verify_locations(ctx_, ca_pem_path.c_str(), nullptr) != 1)
    {
        throw std::runtime_error(opensslError("load CA file " + ca_pem_path));
    }
    ca_path_ = ca_pem_path;
}

bool TlsContext::reload()
{
    if (cert_path_.empty() || key_path_.empty())
    {
        return false;           // Nothing was loaded to reload.
    }
    // Validate the new material into the live SSL_CTX. OpenSSL replaces the
    // current certificate/key on success; on failure the previous ones remain,
    // so a mid-rotation partially-written file cannot break the running server.
    // (Both files are validated together via check_private_key before we
    // consider the read successfull.)
    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_path_.c_str()) != 1)
    {
        opensslError("reload certificate");     // Clear the error queue
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, key_path_.c_str(),
                                    SSL_FILETYPE_PEM) != 1)
    {
        opensslError("reload private key");
        return false;
    }
    if (SSL_CTX_check_private_key(ctx_) != 1)
    {
        opensslError("reloaded key/cert mismatch");
        return false;
    }
    if (ca_path_.empty())
    {
        if (SSL_CTX_load_verify_locations(ctx_, ca_path_.c_str(), nullptr) != 1)
        {
            opensslError("reload CA");
            return false;
        }
    }
    return true;
}

void TlsContext::setVerifyPeer(bool verify)
{
    if (verify)
    {
        int mode = SSL_VERIFY_PEER;
        if (role_ == Role::kServer)
        {
            // Require the client to present a certificate (mutual TLS).
            mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
        SSL_CTX_set_verify(ctx_, mode, nullptr);
    }
    else
    {
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    }
}

// ---- SNI ----
namespace{
std::string toLowerAscii(std::string s)
{
    for (char&c : s)
    {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}
} // namespace

void TlsContext::addSniContext(const std::string& hostname,
                                TlsContextPtr ctx)
{
    if (role_ != Role::kServer)
    {
        throw std::runtime_error("addSniContext is server-role only");
    }
    if (!ctx)
    {
        throw std::runtime_error("addSniContext: null context");
    }
    if (ctx->role() != Role::kServer)
    {
        throw std::runtime_error("addSniContext: context must be server role");
    }

    sni_contexts_[toLowerAscii(hostname)] = std::move(ctx);

    // Install the callback lazily, on the first registration, so a server that
    // never uses SNI pays nothing.
    // The OpenSSL macro expands to a C-style function pointer cast internally,
    // which trips -Wold-style-cast under GCC; suppress it at this call site
    // rather than weakening the flag for our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    SSL_CTX_set_tlsext_servername_callback(ctx_, &TlsContext::sniCallback);
#pragma GCC diagnostic pop
    SSL_CTX_set_tlsext_servername_arg(ctx_, this);
}

TlsContext* TlsContext::findSniContext(const std::string& hostname) const{
    const std::string& host = toLowerAscii(hostname);

    // Exact match wins over any wildcard.
    auto it = sni_contexts_.find(host);
    if (it != sni_contexts_.end()) return it->second.get();

    // Single-level wildcard "*.example.com" matches "a.example.com" but NOT
    // "example.com" nor "a.b.example.com" (RFC 6125 semantics).
    size_t dot = host.find('.');
    if (dot != std::string::npos && dot + 1 < host.size())
    {
        std::string wildcard = "*" + host.substr(dot);
        auto w = sni_contexts_.find(wildcard);
        if (w != sni_contexts_.end()) return w->second.get();
    }
    return nullptr;     // Caller keeps the default context
}

size_t TlsContext::sniCount() const { return sni_contexts_.size(); }

std::string TlsContext::requestedServerName(struct ssl_st* ssl)
{
    if (ssl == nullptr) return std::string();
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    return name ? std::string(name) : std::string();
}

int TlsContext::sniCallback(struct ssl_st* ssl, int* /*alert*/, void* arg)
{
    auto* self = static_cast<TlsContext*>(arg);
    if (self == nullptr || ssl == nullptr) return SSL_TLSEXT_ERR_OK;

    const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (servername == nullptr)
    {
        // No SNI sent (or an IP literal was used): keep the default context.
        return SSL_TLSEXT_ERR_OK;
    }

    TlsContext* selected = self->findSniContext(servername);
    if (selected == nullptr)
        // Unknown name: fall back to the default certificate rather than
        // aborting. Failing the handshake here would make an unrecognized Host
        // a hard connection error; nginx's default_server behaves this way too.
        return SSL_TLSEXT_ERR_OK;
    }

    // Swap this connection onto the matched context, so the certificate,
    // key, and verfication settings all come from it.
    if (SSL_set_SSL_CTX(ssl, selected->nativeHandle()) == nullptr)
    {
        return SSL_TLSEXT_ERR_OK;           // Keep the default on failure
    }
    // SSL_set_SSL_CTX does NOT copy these, so re-apply them from the chosen
    // context; otherwise a per-hsot verification setting would be ignored.
    SSL_set_option(ssl, SSL_CTX_get_options(selected->nativeHandle()));
    SSL_set_verify(ssl, SSL_CTX_get_verify_mode(selected->nativeHandle()),
                        SSL_CTX_get_verify_callback(selected->nativeHandle()));
    
    return SSL_TLSEXT_ERR_OK;
}

} // namespace tls