#include "ocsp_stapler.h"

#include "tls_context.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tls
{
namespace
{

// RAII helpers so every early return cannot leak an OpenSSL object.
struct X509Deleter {
    void operator()(X509* p) const { if (p) X509_free(p); }
};
struct OcspReqDeleter {
    void operator()(OCSP_REQUEST* p) const { if (p) OCSP_REQUEST_free(p); }
};
struct OcspRespDeleter {
    void operator()(OCSP_RESPONSE* p) const { if (p) OCSP_RESPONSE_free(p); }
};
struct OcspBasicDeleter {
    void operator()(OCSP_BASICRESP* p) const { if (p) OCSP_BASICRESP_free(p); }
};
struct BioDeleter {
    void operator()(BIO* p) const { if (p) BIO_free_all(p); }
};

using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using OcspReqPtr = std::unique_ptr<OCSP_REQUEST, OcspReqDeleter>;
using OcspRespPtr = std::unique_ptr<OCSP_RESPONSE, OcspRespDeleter>;
using OcspBasicPtr = std::unique_ptr<OCSP_BASICRESP, OcspBasicDeleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

X509Ptr loadPemCert(const std::string& path)
{
    BioPtr bio(BIO_new_file(path.c_str(), "r"));
    if (!bio) return nullptr;
    return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

// Read the OCSP responder URL from the certificate's Authority Information
// Access extension -- this is how a real cert advertises its responder.
std::string aiaOcspUrl(X509* cert)
{
    std::string url;
    STACK_OF(OPENSSL_STRING)* urls = X509_get1_ocsp(cert);
    if (urls != nullptr)
    {
        if (sk_OPENSSL_STRING_num(urls) > 0)
        {
            const char* u = sk_OPENSSL_STRING_value(urls, 0);
            if (u) url = u;
        }
        X509_email_free(urls);
    }
    return url;
}

// Convert an ASN1_GENERALIZEDTIME to a unix timestamp.
int64_t asn1TimeToEpoch(const ASN1_GENERALIZEDTIME* t)
{
    if (t == nullptr) return 0;
    struct tm tm_time;
    std::memset(&tm_time, 0, sizeof(tm_time));
    if (ASN1_TIME_to_tm(t, &tm_time) != 1) return 0;
    return static_cast<int64_t>(timegm(&tm_time));
}

void clearErrors() { ERR_clear_error(); }
}   // namespace

OcspStapler::OcspStapler(std::string cert_path, std::string issuer_path)
    : cert_path_(std::move(cert_path)), issuer_path_(std::move(issuer_path)) {}

OcspStapler::~OcspStapler() = default;

std::string OcspStapler::responderUrl() const 
{
    if (!responder_url_.empty()) return responder_url_;
    X509Ptr cert = loadPemCert(cert_path_);
    if (!cert) return std::string();
    return aiaOcspUrl(cert.get());
}

bool OcspStapler::setResponseDer(const std::string& der)
{
    if (der.empty()) return false;

    // Parse and sanity-check before caching: serving a malformed or "tryLater"
    // response is worse than serving none, because the client may treat a
    // broken staple as a hard failure.
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.data());
    OcspRespPtr resp(d2i_OCSP_RESPONSE(nullptr, &p,
                                       static_cast<long>(der.size())));
    if (!resp)
    {
        clearErrors();
        return false;
    }
    if (OCSP_response_status(resp.get()) != OCSP_RESPONSE_STATUS_SUCCESSFUL)
    {
        return false;
    }
    OcspBasicPtr basic(OCSP_response_get1_basic(resp.get()));
    if (!basic)
    {
        clearErrors();
        return false;
    }

    // Pull thisUpdate/nextUpdate so we know when the staple goes stale. A
    // response whose validity window has passed must not be served.
    int64_t next_epoch = 0;
    if (OCSP_resp_count(basic.get()) > 0)
    {
        OCSP_SINGLERESP* single = OCSP_resp_get0(basic.get(), 0);
        if (single != nullptr)
        {
            ASN1_GENERALIZEDTIME* this_update = nullptr;
            ASN1_GENERALIZEDTIME* next_update = nullptr;
            int status = V_OCSP_CERTSTATUS_UNKNOWN;
            int reason = 0;
            ASN1_GENERALIZEDTIME* revoked_at = nullptr;
            status = OCSP_single_get0_status(single, &reason, &revoked_at,
                                            &this_update, &next_update);
            // Only a definitive "good" is worth stapling. Stapling a "revoked"
            // response would actively tell clients to reject this server.
            if (status != V_OCSP_CERTSTATUS_GOOD)
            {
                return false;
            }
            // Reject a response that is already outside its validity window.
            if (!OCSP_check_validity(this_update, next_update,
                                     /*nsec=*/300L, /*maxsec=*/-1L))
            {
                clearErrors();
                return false;
            }
            next_epoch = asn1TimeToEpoch(next_update);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    response_der_ = der;
    next_update_epoch_ = next_epoch;
    return true;
}

bool OcspStapler::loadFromFile(const std::string& der_path)
{
    std::FILE* fp = std::fopen(der_path.c_str(), "rbe");
    if (fp == nullptr) return false;
    std::string der;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) der.append(buf, n);

    std::fclose(fp);
    return setResponseDer(der);
}

bool OcspStapler::refresh()
{
    X509Ptr cert = loadPemCert(cert_path_);
    X509Ptr issuer = loadPemCert(issuer_path_);
    if (!cert || !issuer) return false;

    std::string url = responderUrl();
    if (url.empty()) return false;

    // Split the URL into host/port/path as OpenSSL's OCSP client needs.
    char* host = nullptr;
    char* port = nullptr;
    char* path = nullptr;
    int use_ssl = 0;
    if (OCSP_parse_url(url.c_str(), &host, &port, &path, &use_ssl) != 1)
    {
        clearErrors();
        return false;
    }
    struct UrlParts {
        char* host; char* port; char* path;
        ~UrlParts() {
            OPENSSL_free(host); OPENSSL_free(port); OPENSSL_free(path);
        }
    } parts{host, port, path};

    // Build the request: one CertID identifying (issuer, serial).
    OcspReqPtr req(OCSP_REQUEST_new());
    if (!req) return false;
    OCSP_CERTID* id = OCSP_cert_to_id(nullptr, cert.get(), issuer.get());
    if (id == nullptr)
    {
        clearErrors();
        return false;
    }
    if (OCSP_request_add0_id(req.get(), id) == nullptr)
    {
        OCSP_CERTID_free(id);
        return false;
    }
    // A nonce prevents a replayed response, though many public responders
    // pre-sign and reject nonces; failure to match is tolerated below.
    OCSP_request_add1_nonce(req.get(), nullptr, -1);

    BioPtr bio(BIO_new_connect(host));
    if (!bio) return false;
    BIO_set_conn_port(bio.get(), port);
    if (BIO_do_connect(bio.get()) <= 0)
    {
        clearErrors();
        return false;
    }

    OcspRespPtr resp(OCSP_sendreq_bio(bio.get(), path, req.get()));
    if (!resp)
    {
        clearErrors();
        return false;
    }

    // Serialize to DER and validate through the shared path.
    unsigned char* der = nullptr;
    int len = i2d_OCSP_RESPONSE(resp.get(), &der);
    if (len <= 0 || der == nullptr)
    {
        clearErrors();
        return false;
    }

    std::string der_str(reinterpret_cast<char*>(der), static_cast<size_t>(len));
    OPENSSL_free(der);
    return setResponseDer(der_str);
}

std::string OcspStapler::cachedResponse() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return response_der_;
}

bool OcspStapler::hasResponse() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !response_der_.empty();
}

int64_t OcspStapler::secondsUntilExpiry() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (response_der_.empty()) return 0;
    // nextUpdate is OPTIONAL (RFC 6960). Distinguish "absent" (-1) from
    // "expired" (0) so a caller can tell a responder that gives no expiry hint
    // from a staple that is genuinely stale.
    if (next_update_epoch_ == 0) return -1;
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t remaining = next_update_epoch_ - now;
    return remaining > 0 ? remaining : 0;
}

void OcspStapler::attachTo(TlsContext& ctx)
{
    // OpenSSL's macro casts internally; see note in tls_context.cc.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    SSL_CTX_set_tlsext_status_cb(ctx.nativeHandle(),
                                 &OcspStapler::statusCallback);
#pragma GCC diagnostic pop
    SSL_CTX_set_tlsext_status_arg(ctx.nativeHandle(), this);
}

int OcspStapler::statusCallback(struct ssl_st* ssl_raw, void* arg)
{
    SSL* ssl = reinterpret_cast<SSL*>(ssl_raw);
    auto* self = static_cast<OcspStapler*>(arg);
    if (self == nullptr || ssl == nullptr)
    {
        return SSL_TLSEXT_ERR_NOACK;
    }
    std::string der = self->cachedResponse();
    if (der.empty())
    {
        // No staple available: proceed without one (fail open). A responder
        // outage must not break the handshake.
        return SSL_TLSEXT_ERR_NOACK;
    }
    // OpenSSL takes ownership of this buffer and frees it, so it must be
    // OPENSSL_malloc'd, not new'd -- and a fresh copy per handshake.
    unsigned char* buf =
        static_cast<unsigned char*>(OPENSSL_malloc(der.size()));
    if (buf == nullptr) return SSL_TLSEXT_ERR_NOACK;
    std::memcpy(buf, der.data(), der.size());
    if (SSL_set_tlsext_status_ocsp_resp(ssl, buf,
                                        static_cast<int>(der.size())) != 1)
    {
        OPENSSL_free(buf);
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}
}   // namespace tls
