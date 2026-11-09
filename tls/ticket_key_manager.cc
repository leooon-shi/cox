#include "ticket_key_manager.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <cstring>

namespace tls {

TicketKeyManager::TicketKeyManager() {
    rotate();  // start with one current key
}

void TicketKeyManager::rotate() {
    Key k;
    // CSPRNG for all key material. RAND_bytes returns 1 on success; on the
    // vanishingly rare failure we leave the previous keys in place rather than
    // install weak material.
    if (RAND_bytes(k.name.data(), static_cast<int>(k.name.size())) != 1 ||
        RAND_bytes(k.aes_key.data(), static_cast<int>(k.aes_key.size())) != 1 ||
        RAND_bytes(k.hmac_key.data(), static_cast<int>(k.hmac_key.size())) !=
            1) {
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    keys_.push_back(k);
    while (keys_.size() > kMaxKeys) {
        keys_.pop_back();  // retire the oldest
    }
}

size_t TicketKeyManager::keyCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return keys_.size();
}

bool TicketKeyManager::initSeal(const Key& k, unsigned char iv[16],
                                EVP_CIPHER_CTX* ectx, EVP_MAC_CTX* hctx) {
    if (RAND_bytes(iv, 16) != 1) {
        return false;
    }
    if (EVP_EncryptInit_ex(ectx, EVP_aes_256_cbc(), nullptr, k.aes_key.data(),
                           iv) != 1) {
        return false;
    }
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                         const_cast<char*>("SHA256"), 0),
        OSSL_PARAM_construct_end()};
    if (EVP_MAC_init(hctx, k.hmac_key.data(), k.hmac_key.size(), params) != 1) {
        return false;
    }
    return true;
}

bool TicketKeyManager::initOpen(const Key& k, const unsigned char* iv,
                                EVP_CIPHER_CTX* ectx, EVP_MAC_CTX* hctx) {
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                         const_cast<char*>("SHA256"), 0),
        OSSL_PARAM_construct_end()};
    if (EVP_MAC_init(hctx, k.hmac_key.data(), k.hmac_key.size(), params) != 1) {
        return false;
    }
    if (EVP_DecryptInit_ex(ectx, EVP_aes_256_cbc(), nullptr, k.aes_key.data(),
                           iv) != 1) {
        return false;
    }
    return true;
}

int TicketKeyManager::keyCallback(SSL* /*ssl*/, unsigned char key_name[16],
                                  unsigned char* iv, EVP_CIPHER_CTX* ectx,
                                  EVP_MAC_CTX* hctx, int enc) {
    std::lock_guard<std::mutex> lk(mu_);
    if (keys_.empty()) {
        return -1;
    }

    if (enc == 1) {
        // Seal a new ticket with the CURRENT key (front of the deque).
        const Key& cur = keys_.front();
        std::memcpy(key_name, cur.name.data(), cur.name.size());
        if (!initSeal(cur, iv, ectx, hctx)) {
            return -1;
        }
        return 1;
    }

    // enc == 0: decrypt. Find the key whose name matches the ticket.
    for (size_t i = 0; i < keys_.size(); ++i) {
        const Key& k = keys_[i];
        if (std::memcmp(key_name, k.name.data(), k.name.size()) == 0) {
            if (!initOpen(k, iv, ectx, hctx)) {
                return -1;
            }
            // i == 0 is the current key -> 1 (resume, keep ticket)
            // i > 0 is a previous key    -> 2 (resume AND re-issue a fresh
            // ticket sealed under the current key, migrating the client
            // forward before the old key is retired).
            return (i == 0) ? 1 : 2;
        }
    }
    // Unknown key name (retired/forged): no resumption, fall back to a full
    // handshake.
    return 0;
}

}  // namespace tls
