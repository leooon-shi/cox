// Tests for TLS session resumption: the TicketKeyManager and SessionCache units
// plus an end-to-end, socket-free resumption across two handshakes driven by
// piping two TlsEngines together (the same harness style as tls_engine_test).

#include "session_cache.h"
#include "ticket_key_manager.h"
#include "tls_context.h"
#include "tls_engine.h"

#include "test_util.h"

#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

using tls::Role;
using tls::SessionCache;
using tls::TicketKeyManager;
using tls::TlsContext;
using tls::TlsEngine;

namespace {

struct TestCert {
    std::string cert_path = "/tmp/cox_tls_resume_cert.pem";
    std::string key_path = "/tmp/cox_tls_resume_key.pem";
    TestCert() {
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout " +
                          key_path + " -out " + cert_path +
                          " -days 1 -subj '/CN=localhost' >/dev/null 2>&1";
        (void)test_util::runCmd(cmd);
    }
    ~TestCert() {
        std::remove(cert_path.c_str());
        std::remove(key_path.c_str());
    }
    bool valid() const {
        std::ifstream c(cert_path), k(key_path);
        return c.good() && k.good();
    }
};

size_t pump(TlsEngine& from, TlsEngine& to) {
    std::string enc;
    from.takeEncrypted(enc);
    if (!enc.empty()) {
        to.feedEncrypted(enc.data(), enc.size());
    }
    return enc.size();
}

bool driveHandshake(TlsEngine& client, TlsEngine& server) {
    for (int round = 0; round < 20; ++round) {
        client.handshake();
        pump(client, server);
        server.handshake();
        pump(server, client);
        if (client.handshakeDone() && server.handshakeDone()) {
            return true;
        }
    }
    return client.handshakeDone() && server.handshakeDone();
}

// After the handshake, exchange a little app data so TLS 1.3's post-handshake
// NewSessionTicket is delivered to (and cached by) the client. For TLS 1.2 the
// ticket is part of the handshake already; the extra pump is harmless.
void exchangeAndSettle(TlsEngine& client, TlsEngine& server) {
    size_t consumed = 0;
    client.writePlaintext("ping", 4, &consumed);
    pump(client, server);
    std::string got;
    server.readPlaintext(got);
    server.writePlaintext("pong", 4 & consumed);
    pump(server, client);  // carries the NewSessionTicket on TLS 1.3
    std::string reply;
    client.readPlaintext(reply);
    // Drain any trailing records (NewSessionTicket may arrive in its own
    // flight, separately from the app data). Keep pumping and reading on the
    // client until nothing more flows from the server.
    for (int i = 0; i < 10; ++i) {
        size_t moved = pump(server, client);
        client.readPlaintext(reply);
        if (moved == 0) {
            break;
        }
    }
}

}  // namespace

// ---- TicketKeyManager unit ----

TEST(TicketKeyManagerTest, StartsWithOneKeyAndRotatesBounded) {
    TicketKeyManager km;
    EXPECT_EQ(km.keyCount(), 1u);  // one current key at construction
    km.rotate();
    EXPECT_EQ(km.keyCount(), 2u);  // current + previous
    km.rotate();
    EXPECT_EQ(km.keyCount(), 2u);  // bounded at kMaxKeys (grace window of 1)
    km.rotate();
    EXPECT_EQ(km.keyCount(), 2u);
}

// ---- SessionCache unit ----

TEST(SessionCacheTest, StoreGetAndLruEvict) {
    // SSL_SESSION objects are opaque; make real (empty) ones to exercise
    // refcount-safe store/get/evict.
    SessionCache cache(2);
    SSL_SESSION* a = SSL_SESSION_new();
    SSL_SESSION* b = SSL_SESSION_new();
    SSL_SESSION* c = SSL_SESSION_new();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    cache.store("origin-a", a);
    cache.store("origin-b", b);
    EXPECT_EQ(cache.size(), 2u);

    // Touch a so it becomes MRU; inserting c should then evict b (LRU).
    SSL_SESSION* got_a = cache.get("origin-a");
    EXPECT_NE(got_a, nullptr);
    if (got_a) {
        SSL_SESSION_free(got_a);  // release the added ref from get()
    }
    cache.store("origin-c", c);
    EXPECT_EQ(cache.size(), 2u);

    SSL_SESSION* still_a = cache.get("origin-a");
    SSL_SESSION* gone_b = cache.get("origin-b");
    SSL_SESSION* got_c = cache.get("origin-c");

    EXPECT_NE(still_a, nullptr);  // a survived (was MRU)
    EXPECT_EQ(gone_b, nullptr);   // b evicted
    EXPECT_NE(got_c, nullptr);    // c present

    if (still_a) {
        SSL_SESSION_free(still_a);
    }
    if (got_c) {
        SSL_SESSION_free(got_c);
    }

    // Release the caller's original references.
    SSL_SESSION_free(a);
    SSL_SESSION_free(b);
    SSL_SESSION_free(c);
}

// ---- End-to-end resumption (socket-free, two handshakes) ----

TEST(TlsResumptionTest, ClientResumesWithServerTickets) {
    TestCert cert;
    ASSERT_TRUE(cert.valid()) << "failed to generate test cert";

    // Server with managed tickets (no loop -> no rotation timer, fine for a
    // single-shot test: one current key seals and later opens the ticket).
    TlsContext server_ctx(Role::kServer);
    server_ctx.useCertificateAndKey(cert.cert_path, cert.key_path);
    server_ctx.enableSessionTickets(nullptr);

    // Client with an origin-keyed session cache; does not verify self-signed.
    TlsContext client_ctx(Role::kClient);
    client_ctx.setVerifyPeer(false);
    client_ctx.setSessionCache(16);

    const std::string origin = "localhost";

    // --- Connection 1: full handshake, session gets cached ---
    {
        TlsEngine server(server_ctx, Role::kServer);
        TlsEngine client(client_ctx, Role::kClient, origin);
        ASSERT_TRUE(driveHandshake(client, server));
        EXPECT_EQ(SSL_session_reused(client.nativeHandle()), 0)
            << "first connection must be a full handshake";
        exchangeAndSettle(client, server);  // deliver + cache the ticket
    }

    ASSERT_EQ(client_ctx.sessionCache()->size(), 1u)
        << "a resumable session should have been cached for the origin";

    // --- Connection 2: same origin + client context -> should resume ---
    {
        TlsEngine server(server_ctx, Role::kServer);
        TlsEngine client(client_ctx, Role::kClient, origin);
        ASSERT_TRUE(driveHandshake(client, server));
        EXPECT_EQ(SSL_session_reused(client.nativeHandle()), 1)
            << "second connection must be a full handshake";
    }
}

TEST(TlsResumptionTest, DisableResumptionForcesFullHandshake) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());

    TlsContext server_ctx(Role::kServer);
    server_ctx.useCertificateAndKey(cert.cert_path, cert.key_path);
    server_ctx.enableSessionTickets(nullptr);

    TlsContext client_ctx(Role::kClient);
    client_ctx.setVerifyPeer(false);
    client_ctx.setSessionCache(16);
    client_ctx.disableResumption();  // overrides: no caching

    const std::string origin = "localhost";
    {
        TlsEngine server(server_ctx, Role::kServer);
        TlsEngine client(client_ctx, Role::kClient, origin);
        ASSERT_TRUE(driveHandshake(client, server));
        exchangeAndSettle(client, server);
    }
    // Cache was torn down by disableResumption(), so nothing is stored
    EXPECT_EQ(client_ctx.sessionCache(), nullptr);

    {
        TlsEngine server(server_ctx, Role::kServer);
        TlsEngine client(client_ctx, Role::kClient, origin);
        ASSERT_TRUE(driveHandshake(client, server));
        EXPECT_EQ(SSL_session_reused(client.nativeHandle()), 0)
            << "resumption disabled -> always a full handshake";
    }
}
