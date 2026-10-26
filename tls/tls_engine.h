#pragma once

#include "tls_context.h"

#include <cstddef>
#include <string>
#include <vector>

struct ssl_st;
typedef struct ssl_st SSL;
struct bio_st;
typedef struct bio_st BIO;

namespace tls {
// TlsEngine drives a single TLS connection's OpenSSL state machine using memory
// BIOs, with NO knowledge of sockets or event loops. This isolates the tricky
// TLS logic so it can be unit-tested by piping the two engines' network output
// into each other.
//
// Data flows:
//   network (encrypted) --feedEncrypted()--> rbio --> SSL --> readPlaintext()
//   writePlaintext() --> SSL --> wbio -- takeEncrypted()--> network (encrypted)
//
// Typical driving loop (per side):
//   1. call handshake() until handshakeDone(); after each call, flush
//      takeEncrypted() to the network and feed any received bytes with
//      feedEncrypted().
//   2. once established, writePlaintext()/readPlaintext() for app data, always
//      flushing takeEncrypted() to the network and feeding inbound bytes.
class TlsEngine {
public:
    enum class Status { kOk, kWantIo, kClosed, kError };

    TlsEngine(const TlsContext& ctx, Role role);
    // Client-role overload: `server_hostname` is sent as the SNI extension so a
    // multi-domain server can pick the right certificate (RFC 6066), and is
    // additionally used for certificate hostname verification. Pass "" or use
    // the other constructor to omit SNI. Ignored for the server role.
    TlsEngine(const TlsContext& ctx, Role role,
              const std::string& server_hostname);
    ~TlsEngine();

    TlsEngine(const TlsEngine&) = delete;
    TlsEngine& operator=(const TlsEngine&) = delete;

    // Feed encrypted bytes received from the network into the TLS engine.
    // Returns the number of bytes consumed (normally all).
    size_t feedEncrypted(const char* data, size_t len);

    // Take encrypted bytes the engine wants to send to the network. Appends to
    // `out`. Returns bytes produced this call. Call in a loop until it returns
    // 0.
    size_t takeEncrypted(std::string& out);

    // Advance the handshake. Returns kOk when the handshake just completed or
    // is complete, kWantIo when more network I/O is needed, kError on failure.
    Status handshake();
    bool handshakeDone() const;

    // Encrypt and queue application plaintext. Returns kOk / kWantIo / kError.
    // On kOk, `consumed` is set to how many bytes were accepted.
    Status writePlaintext(const char* data, size_t len, size_t* consumed);

    // Decrypt available application plaintext into `out` (appended). Returns
    // kOk (data may be 0 bytes), kWantIo (need more network bytes), kClosed
    // (peer sent close_notify), or kError.
    Status readPlaintext(std::string& out);

    // Initiate a clean TLS shutdown (sends close_notify). Flush takeEncrypted()
    // afterwards.
    void shutdown();

    // Negotiated protocol/cipher, for logging (valid after handshake)
    std::string protocolVersion() const;
    std::string cipherName() const;

    // Server role: the hostname the client requested via SNI, or "" if none.
    std::string requestedServerName() const;

    // Underlying OpenSSL SSL object. Exposed for features that need direct
    // access (e.g. requesting/reading an OCSP staple); prefer the wrappers.
    SSL* nativeHandle() const { return ssl_; }

    std::string lastError() const { return last_error_; }

private:
    Status mapError(int ret);
    void init(const TlsContext& ctx, Role role,
              const std::string& server_hostname);

    SSL* ssl_;
    BIO* rbio_;  // network -> SSL (we write inbound encrypted bytes here)
    BIO* wbio_;  // SSL -> network ( we read outbound encrypted bytes here)
    // NOTE: the role is applied at construction (accept vs connect state, SNI)
    // and never needed afterwards, so it is deliberately NOT stored.
    std::string last_error_;
};
}  // namespace tls
