#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>

struct ssl_st;
typedef struct ssl_st SSL;
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
struct hmac_ctx_st;
typedef struct hmac_ctx_st HMAC_CTX;
struct evp_mac_ctx_st;
typedef struct evp_mac_ctx_st EVP_MAC_CTX;

namespace tls {

// TicketKeyManager owns the server-side TLS session-ticket keys and implements
// the OpenSSL ticket-key callback (SSL_CTX_set_tlsext_ticket_key_evp_cb).
//
// A session ticket is the session state sealed (AES-256-CBC) and authenticated
// (HMAC-SHA256) with a server-held key, handed to the client; presenting it
// lets the server resume WITHOUT storing per-session state. The security of
// resumption therefore rests on these keys:
//
//   * A ticket key is a long-lived secret that can decrypt EVERY session
//     resumed under it. If it never rotates, a single key compromise breaks
//     forward secrecy for all resumed sessions over the whole process lifetime.
//   * OpenSSL auto-generates ONE random key and never rotates it. This class
//     fixes that: rotate() installs a fresh current key and retains the
//     immediately previous key for a grace window so tickets issued just before
//     a rotation still resume (and are re-issued under the new key).
//
// Keys live in memory only (CSPRNG-generated); nothing touches disk. Thread-
// safe: the callback runs on an I/O loop thread while rotate() may run on
// another (a timer/worker), so the key deque is mutex-guarded. The metric-style
// hot path is a short critical section copying key bytes, not crypto.
class TicketKeyManager {
public:
    // Number of 16-byte key "names" + 32-byte AES + 32-byte HMAC keys retained:
    // the current key plus the previous one (grace window). Two is enough to
    // avoid a resumption cliff at the rotation instant.
    static constexpr size_t kMaxKeys = 2;

    TicketKeyManager();

    TicketKeyManager(const TicketKeyManager&) = delete;
    TicketKeyManager& operator=(const TicketKeyManager&) = delete;

    // Generate a fresh current key and retire the oldest if over kMaxKeys.
    // Call on a timer for forward secrecy; also called once at construction.
    void rotate();

    // Number of keys currently retained (for tests/metrics).
    size_t keyCount() const;

    // The OpenSSL EVP ticket-key callback. Installed via
    // SSL_CTX_set_tlsext_ticket_key_evp_cb with `this` reachable through the
    // SSL_CTX app-data. `enc == 1`: seal a new ticket with the current key
    // (fills `key_name` and `iv`). `enc == 0`: find the key by `key_name` and
    // set up decryption; returns 2 for a previous (non-current) key to tell
    // OpenSSL to re-issue the ticket under the current key, 1 for the current
    // key, 0 if unknown (no resumption -> full handshake), -1 on error.
    int keyCallback(SSL* ssl, unsigned char key_name[16], unsigned char* iv,
                    EVP_CIPHER_CTX* ectx, EVP_MAC_CTX* hctx, int enc);

private:
    struct Key {
        std::array<unsigned char, 16> name;      // ticket key identifier
        std::array<unsigned char, 32> aes_key;   // AES-256-CBC
        std::array<unsigned char, 32> hmac_key;  // HMAC-SHA256
    };

    // Set up `ectx`/`hctx` to encrypt+authenticate under `k` (enc path), or to
    // decrypt+verify under `k` (dec path). Returns true on success.
    static bool initSeal(const Key& k, unsigned char iv[16],
                         EVP_CIPHER_CTX* ectx, EVP_MAC_CTX* hctx);
    static bool initOpen(const Key& k, const unsigned char* iv,
                         EVP_CIPHER_CTX* ectx, EVP_MAC_CTX* hctx);

    mutable std::mutex mu_;
    // Front is the current key; back is the oldest retained. Bounded to
    // kMaxKeys by rotate().
    std::deque<Key> keys_;
};

}  // namespace tls
