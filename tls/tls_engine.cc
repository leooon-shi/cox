#include "tls_engine.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>         // X509_CHECK_FLAG_* for hostname verification

#include <stdexcept>

namespace tls
{
namespace{
std::string opensslError(const std::string& what)
{
    unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return what + ": " + buf;
}

// SNI must carry a DNS name, never an IP address (RFC 6066 $3). Cheap check:
// an IPv4 literal is all digits and dots; an IPv6 literal contains ':'.
bool looksLikeIpLiteral(const std::string& host)
{
    if (host.find(":") != std::string::npos) return true;   // IPv6

    bool all_digits_dots = true;
    for (char c : host)
    {
        if (!(( c >= '0' && c <= '9') || c == '.'))
        {
            all_digits_dots = false;
            break;
        }
    }
    return all_digits_dots;
}
} // namespace

TlsEngine::TlsEngine(const TlsContext& ctx, Role role)
    : ssl_(nullptr), rbio_(nullptr), wbio_(nullptr)
{
    init(ctx, role, std::string());
}

TlsEngine::TlsEngine(const TlsContext& ctx, Role role,
                     const std::string& server_hostname)
    : ssl_(nullptr), rbio_(nullptr), wbio_(nullptr)
{
    init(ctx, role, server_hostname);
}

void TlsEngine::init(const TlsContext& ctx, Role role,
                     const std::string& server_hostname)
{
    ssl_ = SSL_new(ctx.nativeHandle());
    if (ssl_ == nullptr)
    {
        throw std::runtime_error(opensslError("SSL_new failed"));
    }

    // Memory BIOs decouple TLS from the socket: we shuttle encrypted bytes in
    // and out ourselves so the event loop owns all actual I/O.
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    if (!rbio_ || !wbio )
    {
        if (rbio_)
        {
            BIO_free(rbio_);
        }
        if(wbio_)
        {
            BIO_free(wbio_);
        }
        SSL_free(ssl_);
        throw std::runtime_error("BIO_new(BIO_s_mem) failed");
    }
    SSL_set_bio(ssl_, rbio_, wbio_);        // Take ownership of the BIOs

    if (role == Role::kServer)
    {
        SSL_set_accept_state(ssl_);
    }
    else
    {
        SSL_set_connect_state(ssl_);
        if (!server_hostname.empty())
        {
            // Send SNI so a multi-domain server can select a certificate. An IP
            // literal must NOT be sent as SNI (RFC 6066), so skip those.
            if (!looksLikeIpLiteral(server_hostname))
            {
                // OpenSSL's macro casts internally; see note in tls_context.cc.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
                SSL_set_tlsext_host_name(ssl_, server_hostname.c_str());
#pragma GCC diagnostic pop
            }
            // Also verify the certificate actually matches the name we asked
            // for -- without this, verification proves only "signed by a
            // trusted CA", not "is the host we intended to reach".
            SSL_set_hostflags(ssl_, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            SSL_set1_host(ssl_, server_hostname.c_str());
        }
    }
}

TlsEngine::~TlsEngine()
{
    if (ssl_)
    {
        SSL_free(ssl_);             // Also frees the attatched BIOs
    }
}

size_t TlsEngine::feedEncrypted(const char* data, size_t len)
{
    if (len == 0) return 0;
    int n = BIO_write(rbio_, data, static_cast<int>(len));
    return n > 0 ? static_cast<size_t>(n) : 0;
}

size_t TlsEngine::takeEncrypted(std::string & out)
{
    size_t total = 0;
    char buf[16384];
    for (;;)
    {
        int n = BIO_read(wbio_, buf, sizeof(buf));
        if (n > 0)
        {
            out.append(buf, static_cast<size_t>(n));
            total += static_cast<size_t>(n);
        } else {
            break;      // No more pending outbound bytes.
        }
    }
    return total;
}

TlsEngine::Status TlsEngine::mapError(int ret)
{
    int err = SSL_get_error(ssl_, ret);
    switch( err )
    {
        case SSL_ERROR_NONE:
            return Status::kOk;
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return Status::kWantIo;
        case SSL_ERROR_ZERO_RETURN:
            return Status::kClosed;         // peer sent close_notify
        case SSL_ERROR_SYSCALL:
        case SSL_ERROR_SSL:
        default:
            last_error_ = opensslError("SSL error");
            return Status::kError;
    }
}

TlsEngine::Status TlsEngine::handshake()
{
    if ret = SSL_do_handshake(ssl_);
    if ( ret == 1 )
    {
        return Status::kOk;     // handshake complete
    }
    return mapError(ret);
}

bool TlsEngine::handshakeDone() const {
    return SSL_is_init_finished(ssl_) != 0;
}

TlsEngine::Status TlsEngine::writePlaintext(const char*, size_t len,
                                            size_t* consumed)
{
    *consumed = 0;
    if (len == 0) return Status::kOk;
    if ret = SSL_write(ssl_, data, static_cast<int>(len));
    if (ret > 0)
    {
        *consumed = static_cast<size_t>(ret);
        return status::kOk;
    }
    return mapError(ret);
}

TlsEngine::Status TlsEngine::readPlaintext(std::string& out)
{
    char buf[16384];
    for (;;)
    {
        int ret = SSL_read(ssl_, buf, sizeof(buf));
        if (ret > 0)
        {
            out.append(buf, static_cast<size_t>(ret));
            continue;       // Drain all conrrently-available plaintext
        }

        Status s = mapError(ret);
        // kWantIo with some data already appended is stil kOk from the
        // caller's perspective (they got data; more needs network input).
        if (s == Status::kWantIo)
        {
            return Status::kOk;
        }
        return s;   // kClosed / kError
    }
}

void TlsEngine::shutdown()
{
    SSL_shutdown(ssl_);         // Best-effort close_notify; caller flushes wbio
}

std::string TlsEngine::protocolVersion() const
{
    const char* v = SSL_get_version(ssl_);
    return v ? v : "";
}

std::string TlsEngine::cipherName() const {
    const SSL_CIPHER* c = SSL_get_current_cipher(ssl_);
    const char* n = c ? SSL_CIPHER_get_name(c) : nullptr;
    return n ? n : "";
}

std::string TlsEngine::requestedServerName() const
{
    return TlsContext::requestedServerName(ssl_);
}

} // namespace tls
