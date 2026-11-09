#include "session_cache.h"

#include <openssl/ssl.h>

namespace tls {

SessionCache::SessionCache(size_t max_sessions)
    : max_(max_sessions == 0 ? 1 : max_sessions) {}

SessionCache::~SessionCache() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [origin, sess] : lru_) {
        SSL_SESSION_free(sess);
    }
}

void SessionCache::dropLocked(SSL_SESSION* s) {
    if (s != nullptr) {
        SSL_SESSION_free(s);
    }
}

void SessionCache::store(const std::string& origin, SSL_SESSION* session) {
    if (session == nullptr) {
        return;
    }
    // Store an INDEPENDENT copy rather than just up-ref'ing the caller's
    // session. For TLS 1.3, OpenSSL marks a session non-resumable once the
    // SSL connection it originated from is freed -- so a mere up-ref would
    // leave us holding a session that silently stops offering a PSK the moment
    // the minting connection closes. A connection pool reconnects on a NEW
    // connection after the old one is gone, which is exactly that case.
    // SSL_SESSION_dup() yields a standalone session whose resumability is not
    // tied to any live SSL, so the cached ticket survives the pool churn.
    SSL_SESSION* copy = SSL_SESSION_dup(session);
    if (copy == nullptr) {
        return;  // allocation/dup failure: skip caching rather than store junk
    }

    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(origin);
    if (it != index_.end()) {
        // Replace the existing session for this origin; drop the old ref.
        dropLocked(it->second->second);
        it->second->second = copy;
        lru_.splice(lru_.begin(), lru_, it->second);  // move to front (MRU)
        return;
    }
    // Insert new; evict LRU (back) if at capacity.
    if (lru_.size() >= max_) {
        auto& victim = lru_.back();
        dropLocked(victim.second);
        index_.erase(victim.first);
        lru_.pop_back();
    }
    lru_.emplace_front(origin, copy);
    index_[origin] = lru_.begin();
}

SSL_SESSION* SessionCache::get(const std::string& origin) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(origin);
    if (it == index_.end()) {
        return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second);  // mark MRU
    SSL_SESSION* s = it->second->second;
    // Hand the caller an added reference: SSL_set_session does NOT take
    // ownership, and the caller frees after setting it.
    SSL_SESSION_up_ref(s);
    return s;
}

size_t SessionCache::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lru_.size();
}

}  // namespace tls
