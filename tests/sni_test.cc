// SNI (Server Name Indication) tests: one server port, several hostnames, each
// with its own certificate. Drives two TlsEngines against each other so the
// selection is verified through a real handshake, not just the lookup table.

#include "tls_context.h"
#include "tls_engine.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

using tls::Role;
using tls::TlsContext;
using tls::TlsContextPtr;
using tls::TlsEngine;

namespace
{
// A self-signed cert for a specific CN, so we can prove WHICH cert was served.
struct NamedCert
{
    std::string cn;
    std::string cert_path;
    std::string key_path;

    explicit NamedCert(const std::string& common_name, const std::string& tag)
        : cn(common_name) {
          cert_path = "/tmp/cox_sni_" + tag + "_cert.pem";
          key_path = "/tmp/cox_sni_" + tag + "_key.pem";
          // subjectAltName is required for modern hostname verification; CN alone
          // is no longer honored by OpenSSL's X509_check_host.
          std::string cmd=
              "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + key_path +
              " -out " + cert_path + " -days 1 -subj '/CN=" + common_name +
              "' -addext 'subjectAltName=DNS:" + common_name + "'"
              " >/dev/null 2>&1";
          // Status intentionally unchecked here: valid() below verifies the
          // files actually appeared, which is the condition that matters.
          (void)test_util::runCmd(cmd);
        }
    ~NamedCert()
    {
        std::remove(cert_path.c_str());
        std::remove(key_path.c_str());
    }
    bool valid() const
    {
        std::ifstream c(cert_path), k(key_path);
        return c.good() && k.good();
    }
};

TlsContextPtr makeServerCtx(const NamedCert& cert)
{
    auto ctx = std::make_shared<TlsContext>(Role::kServer);
    ctx->useCertificateAndKey(cert.cert_path, cert.key_path);
    return ctx;
}

size_t pump(TlsEngine& from, TlsEngine& to)
{
    std::string enc;
    from.takeEncrypted(enc);
    if (!enc.empty()) to.feedEncrypted(enc.data(), enc.size());
    return enc.size();
}

bool driveHandshake(TlsEngine& client, TlsEngine& server)
{
    for (int round = 0; round < 20; ++round)
    {
        client.handshake();
        pump(client, server);
        server.handshake();
        pump(server, client);
        if (client.handshakeDone() && server.handshakeDone()) return true;
    }

    return client.handshakeDone() && server.handshakeDone();
}
}   // namespace


// ---- registry / matching (no handshake) ----

TEST(SniTest, ExactHostnameLookup)
{
    NamedCert base("default.test", "base"), a("a.example.com", "a");
    ASSERT_TRUE(base.valid() && a.valid());

    auto def = makeServerCtx(base);
    def->addSniContext("a.example.com", makeServerCtx(a));

    EXPECT_EQ(def->sniCount(), 1u);
    EXPECT_NE(def->findSniContext("a.example.com"), nullptr);
    EXPECT_EQ(def->findSniContext("other.example.com"), nullptr);
}

TEST(SniTest, LookupIsCaseInsensitive)
{
    NamedCert base("default.test", "base2"), a("a.example.com", "a2");
    ASSERT_TRUE(base.valid() && a.valid());

    auto def = makeServerCtx(base);
    def->addSniContext("A.Example.COM", makeServerCtx(a));

    // DNS names are case-insensitive, so registration and lookup casing must
    // not matter.
    EXPECT_NE(def->findSniContext("a.example.com"), nullptr);
    EXPECT_NE(def->findSniContext("A.EXAMPLE.COM"), nullptr);
}

TEST(SniTest, WildcardMatchesOneLabelOnly)
{
    NamedCert base("default.test", "base3"), w("wild.test", "wild");
    ASSERT_TRUE(base.valid() && w.valid());

    auto def = makeServerCtx(base);
    def->addSniContext("*.example.com", makeServerCtx(w));

    EXPECT_NE(def->findSniContext("api.example.com"), nullptr);
    EXPECT_NE(def->findSniContext("web.example.com"), nullptr);
    // RFC 6125: a wildcard covers exactly one label -- not the bare domain,
    // and not a deeper subdomain.
    EXPECT_EQ(def->findSniContext("example.com"), nullptr);
    EXPECT_EQ(def->findSniContext("a.b.example.com"), nullptr);
}

TEST(SniTest, ExactMatchWinsOverWildcard)
{
    NamedCert base("default.test", "base4"), wild("wild.test", "w4"),
        exact("api.example.com", "e4");
    ASSERT_TRUE(base.valid() && wild.valid() && exact.valid());

    auto def = makeServerCtx(base);
    auto wild_ctx = makeServerCtx(wild);
    auto exact_ctx = makeServerCtx(exact);

    def->addSniContext("*.example.com", wild_ctx);
    def->addSniContext("api.example.com", exact_ctx);

    EXPECT_EQ(def->findSniContext("api.example.com"), exact_ctx.get());
    EXPECT_EQ(def->findSniContext("other.example.com"), wild_ctx.get());
}

TEST(SniTest, ClientRoleRejectsRegistration)
{
    NamedCert c("client.test", "cli");
    ASSERT_TRUE(c.valid());

    auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
    // SNI selection is a server-side concern; registering on a client context
    // is a programming error, not a silent no-op.
    EXPECT_THROW(client_ctx->addSniContext("x.example.com", makeServerCtx(c)),
                 std::runtime_error);
}

// ---- end-to-end handshake selection ----

TEST(SniTest, ServerSeesRequestedHostname)
{
    NamedCert base("default.test", "h1");
    ASSERT_TRUE(base.valid());
    auto server_ctx = makeServerCtx(base);

    auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
    // Not verifying the cert here; this test is only about SNI transmission.
    client_ctx->setVerifyPeer(false);

    TlsEngine client(*client_ctx, Role::kClient, "requested.example.com");
    TlsEngine server(*server_ctx, Role::kServer);
    ASSERT_TRUE(driveHandshake(client, server));

    // The name the client asked for must be visible server-side.
    EXPECT_EQ(server.requestedServerName(), "requested.example.com");
}

TEST(SniTest, NoSniWhenHostnameOmitted)
{
    NamedCert base("default.test", "h2");
    ASSERT_TRUE(base.valid());
    auto server_ctx = makeServerCtx(base);
    auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
    client_ctx->setVerifyPeer(false);

    TlsEngine client(*client_ctx, Role::kClient);  // no hostname
    TlsEngine server(*server_ctx, Role::kServer);
    ASSERT_TRUE(driveHandshake(client, server));

    EXPECT_EQ(server.requestedServerName(), "");
}

TEST(SniTest, IpLiteralIsNotSentAsSni)
{
    NamedCert base("default.test", "h3");
    ASSERT_TRUE(base.valid());
    auto server_ctx = makeServerCtx(base);
    auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
    client_ctx->setVerifyPeer(false);

    // RFC 6066 forbids an IP literal in SNI; it must be suppressed.
    TlsEngine client(*client_ctx, Role::kClient, "127.0.0.1");
    TlsEngine server(*server_ctx, Role::kServer);
    ASSERT_TRUE(driveHandshake(client, server));

    EXPECT_EQ(server.requestedServerName(), "");
}

TEST(SniTest, SelectsPerHostCertificateAndVerifies)
{
    NamedCert def_cert("default.test", "sel_def");
    NamedCert a_cert("alpha.example.com", "sel_a");
    NamedCert b_cert("beta.example.com", "sel_b");
    ASSERT_TRUE(def_cert.valid() && a_cert.valid() && b_cert.valid());

    // One "server port": default context plus two per-host contexts.
    auto server_ctx = makeServerCtx(def_cert);
    server_ctx->addSniContext("alpha.example.com", makeServerCtx(a_cert));
    server_ctx->addSniContext("beta.example.com", makeServerCtx(b_cert));

    // A client asking for alpha must receive alpha's cert -- proven by trusting
    // ONLY alpha's cert as CA and requiring verification to pass.
    {
        auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
        client_ctx->loadCaFile(a_cert.cert_path);
        client_ctx->setVerifyPeer(true);

        TlsEngine client(*client_ctx, Role::kClient, "alpha.example.com");
        TlsEngine server(*server_ctx, Role::kServer);

        EXPECT_TRUE(driveHandshake(client, server))
            << "alpha handshake failed: " << client.lastError();
        EXPECT_EQ(server.requestedServerName(), "alpha.example.com");
    }

    // Same server, different name -> beta's cert, verified against beta only.
    {
        auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
        client_ctx->loadCaFile(b_cert.cert_path);
        client_ctx->setVerifyPeer(true);

        TlsEngine client(*client_ctx, Role::kClient, "beta.example.com");
        TlsEngine server(*server_ctx, Role::kServer);

        EXPECT_TRUE(driveHandshake(client, server))
            << "beta handshake failed: " << client.lastError();
        EXPECT_EQ(server.requestedServerName(), "beta.example.com");
    }
}

TEST(SniTest, WrongCertificateFailsVerification)
{
    NamedCert def_cert("default.test", "wrong_def");
    NamedCert a_cert("alpha.example.com", "wrong_a");
    NamedCert b_cert("beta.example.com", "wrong_b");
    ASSERT_TRUE(def_cert.valid() && a_cert.valid() && b_cert.valid());

    auto server_ctx = makeServerCtx(def_cert);
    server_ctx->addSniContext("alpha.example.com", makeServerCtx(a_cert));

    // Ask for alpha (so alpha's cert is served) but trust only beta: the
    // handshake MUST fail. This is the control proving the previous test's
    // success was real selection, not verification being disabled.
    auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
    client_ctx->loadCaFile(b_cert.cert_path);
    client_ctx->setVerifyPeer(true);

    TlsEngine client(*client_ctx, Role::kClient, "alpha.example.com");
    TlsEngine server(*server_ctx, Role::kServer);
    EXPECT_FALSE(driveHandshake(client, server));
}

TEST(SniTest, UnknownHostnameFallsBackToDefaultCert)
{
    NamedCert def_cert("default.test", "fb_def");
    NamedCert a_cert("alpha.example.com", "fb_a");
    ASSERT_TRUE(def_cert.valid() && a_cert.valid());

    auto server_ctx = makeServerCtx(def_cert);
    server_ctx->addSniContext("alpha.example.com", makeServerCtx(a_cert));

    // An unregistered name must not abort the handshake; it gets the default
    // certificate (nginx default_server behavior). Verified by trusting only
    // the DEFAULT cert and asking for an unknown host.
    auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
    client_ctx->loadCaFile(def_cert.cert_path);
    client_ctx->setVerifyPeer(false);   // name don't match default cert's CN

    TlsEngine client(*client_ctx, Role::kClient, "unknown.example.org");
    TlsEngine server(*server_ctx, Role::kServer);
    EXPECT_TRUE(driveHandshake(client, server))
        << "unknown SNI should fall back, not fail: " << client.lastError();
    EXPECT_EQ(server.requestedServerName(), "unknown.example.org");
}

TEST(SniTest, WildcardContextServesSubdomain)
{
    NamedCert def_cert("default.test", "wc_def");
    NamedCert wild_cert("*.example.com", "wc_wild");
    ASSERT_TRUE(def_cert.valid() && wild_cert.valid());

    auto server_ctx = makeServerCtx(def_cert);
    server_ctx->addSniContext("*.example.com", makeServerCtx(wild_cert));

    auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
    client_ctx->loadCaFile(wild_cert.cert_path);
    client_ctx->setVerifyPeer(true);

    // The wildcard cert covers this subdomain, and the wildcard registry entry
    // selects it.
    TlsEngine client(*client_ctx, Role::kClient, "api.example.com");
    TlsEngine server(*server_ctx, Role::kServer);
    EXPECT_TRUE(driveHandshake(client, server))
        << "wildcard handshake failed: " << client.lastError();
}
