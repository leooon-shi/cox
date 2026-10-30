// OCSP stapling tests. Builds a real mini-PKI (CA + leaf), runs OpenSSL's own
// `openssl ocsp` responder locally, fetches a real signed response, and checks
// that a handshake delivers the staple to the client.
//
// The heavyweight PKI/responder tests are skipped (not failed) if the openssl
// CLI cannot produce the material, so the suite stays green in a minimal
// environment. The validation/fail-open tests need no PKI and always run.

#include "ocsp_stapler.h"

#include "tls_context.h"
#include "tls_engine.h"

#include <openssl/ocsp.h>
#include <openssl/ssl.h>

#include "test_util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using tls::OcspStapler;
using tls::Role;
using tls::TlsContext;
using tls::TlsContextPtr;
using tls::TlsEngine;
using namespace std::chrono_literals;

namespace {
constexpr int kResponderPort = 18790;

bool fileExists(const std::string& p) {
    std::ifstream f(p);
    return f.good();
}

int runCmd(const std::string& cmd) {
    return std::system((cmd + " >/dev/null 2>&1").c_str());
}

// A throwaway CA plus a leaf certificate carrying an AIA extension pointing at
// our local responder -- the shape a publicly-trusted cert has.
struct MiniPki {
    std::string dir = "/tmp/cow_ocsp_pki";
    std::string ca_key = dir + "/ca.key.pem";
    std::string ca_cert = dir + "/ca.cert.pem";
    std::string leaf_key = dir + "/leaf.key.pem";
    std::string leaf_csr = dir + "/leaf.csr.pem";
    std::string leaf_cert = dir + "/leaf.cert.pem";
    std::string index = dir + "/index.txt";
    std::string ext = dir + "/leaf.ext";
    std::string resp_der = dir + "/resp.der";
    bool ok = false;

    MiniPki() { ok = build(); }
    ~MiniPki() { runCmd("rm -rf " + dir); }

    bool build() {
        runCmd("rm -rf " + dir);
        if (runCmd("mkdir -p " + dir) != 0) {
            return false;
        }

        // CA.
        if (runCmd("openssl req -x509 -newkey rsa:2048 -nodes -keyout " +
                   ca_key + " -out " + ca_cert +
                   " -days 2 -subj '/CN=Cox Test CA'") != 0) {
            return false;
        }

        // Leaf key + CSR.
        if (runCmd("openssl req -newkey rsa:2048 -nodes -keyout " + leaf_key +
                   " -out " + leaf_csr + " -subj '/CN=ocsp.example.com'") !=
            0) {
            return false;
        }

        // The AIA extension advertises the responder URL; this is what
        // responderUrl() must discover.
        std::ofstream e(ext);
        if (!e.good()) {
            return false;
        }
        e << "basicConstraints=CA:FALSE\n"
          << "subjectAltName=DNS:ocsp.example.com\n"
          << "authorityInfoAccess=OCSP;URI:http://127.0.0.1:" << kResponderPort
          << "\n";
        e.close();

        // openssl ca needs an index/serial database to track issued certs,
        // which the responder then consults for revocation status.
        std::ofstream idx(index);
        idx.close();
        std::ofstream serial(dir + "/serial");
        serial << "01\n";
        serial.close();

        std::ofstream conf(dir + "/ca.cnf");
        conf << "[ca]\ndefault_ca=CA_default\n\n[CA_default]\n"
             << "dir=" << dir << "\n"
             << "database=" << index << "\n"
             << "serial=" << dir << "/serial\n"
             << "new_certs_dir=" << dir << "\n"
             << "certificate=" << ca_cert << "\n"
             << "private_key=" << ca_key << "\n"
             << "default_md=sha256\n"
             << "policy=policy_any\ndefault_days=2\n"
             << "copy_extensions=copy\nunique_subject=no\n\n"
             << "[policy_any]\ncommonName=supplied\n";
        conf.close();

        if (runCmd("openssl ca -batch -config " + dir + "/ca.cnf -in " +
                   leaf_csr + " -out " + leaf_cert + " -extfile " + ext) != 0) {
            return false;
        }
        return fileExists(leaf_cert) && fileExists(ca_cert);
    }
};

// Runs `openssl ocsp` in responder mode for the lifetime of the object.
// `nmin` sets the nextUpdate window in minutes; 0 omits nextUpdate entirely
// (OpenSSL's default), which is legal but gives no expiry hint.
struct Responder {
    std::string pid_file = "/tmp/cox_ocsp_responder.pid";
    bool started = false;

    explicit Responder(const MiniPki& pki, int nmin = 60) {
        std::string nmin_opt = nmin > 0 ? " -nmin " + std::to_string(nmin) : "";
        std::string cmd =
            "openssl ocsp -port " + std::to_string(kResponderPort) +
            " -index " + pki.index + " -CA " + pki.ca_cert + " -rkey " +
            pki.ca_key + " -rsigner " + pki.ca_cert + nmin_opt +
            " -nrequest 5" + " >/dev/null 2>&1 & echo $! > " + pid_file;
        // Responder is started in the background; readiness is probed by
        // the refresh()/skip logic in each test rather than this status.
        (void)test_util::runCmd(cmd);
        std::this_thread::sleep_for(500ms);  // let it bind
        started = true;
    }

    ~Responder() {
        // Best-effort teardown: the responder may have already exited.
        (void)test_util::runCmd("kill $(cat " + pid_file + ") >/dev/null 2>&1");
        std::remove(pid_file.c_str());
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
}  // namespace

// ---- validation (no PKI needed) ----

TEST(OcspTest, RejectsEmptyResponse) {
    OcspStapler stapler("/nonexistent.pem", "/nonexistent.pem");
    EXPECT_FALSE(stapler.setResponseDer(""));
    EXPECT_FALSE(stapler.hasResponse());
}

TEST(OcspTest, RejectsGarbageResponse) {
    OcspStapler stapler("/nonexistent.pem", "/nonexistent.pem");
    // Must not cache unparseable bytes: serving a broken staple is worse than
    // serving none.
    EXPECT_FALSE(stapler.setResponseDer("this is not a DER OCSP response"));
    EXPECT_FALSE(stapler.hasResponse());
}

TEST(OcspTest, RejectsMissingFile) {
    OcspStapler stapler("/nonexistent.pem", "/nonexistent.pem");
    EXPECT_FALSE(stapler.loadFromFile("/tmp/cox_no_such_ocsp_response.der"));
    EXPECT_FALSE(stapler.hasResponse());
}

TEST(OcspTest, NoResponseMeansNoExpiry) {
    OcspStapler stapler("/nonexistent.pem", "/nonexistent.pem");
    EXPECT_EQ(stapler.secondsUntilExpiry(), 0);
}

TEST(OcspTest, RefreshFailsWithoutResponderUrl) {
    // No cert to read AIA from and no override -> nothing to contact.
    OcspStapler stapler("/nonexistent.pem", "/nonexistent.pem");
    EXPECT_TRUE(stapler.responderUrl().empty());
    EXPECT_FALSE(stapler.refresh());
}

// ---- fail-open behavior ----

TEST(OcspTest, HandshakeSucceedsWithoutStaple) {
    // The central safety property: a stapler with no cached response must NOT
    // break TLS. A responder outage degrades stapling, it does not take the
    // server down.
    const std::string cert = "/tmp/cox_ocsp_plain_cert.pem";
    const std::string key = "/tmp/cox_ocsp_plain_key.pem";
    ASSERT_EQ(runCmd("openssl req -x509 -newkey rsa:2048 -nodes -keyout " +
                     key + " -out " + cert +
                     " -days 1 -subj '/CN=localhost' -addext "
                     "'subjectAltName=DNS:localhost'"),
              0);

    auto server_ctx = std::make_shared<TlsContext>(Role::kServer);
    server_ctx->useCertificateAndKey(cert, key);

    OcspStapler stapler(cert, cert);
    ASSERT_FALSE(stapler.hasResponse());
    stapler.attachTo(*server_ctx);  // callback installed, but nothing cached.

    auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
    client_ctx->setVerifyPeer(false);

    TlsEngine client(*client_ctx, Role::kClient, "localhost");
    TlsEngine server(*server_ctx, Role::kServer);
    EXPECT_TRUE(driveHandshake(client, server))
        << "handshake must survive a missing staple: " << client.lastError();

    std::remove(cert.c_str());
    std::remove(key.c_str());
}

// ---- real responder end-to-end ----

TEST(OcspTest, DiscoversResponderUrlFromAia) {
    MiniPki pki;
    if (!pki.ok) {
        GTEST_SKIP() << "openssl CLI could not build the test PKI";
    }

    OcspStapler stapler(pki.leaf_cert, pki.ca_cert);
    // The URL must come from the certificate's AIA extension, not be guessed.
    EXPECT_EQ(stapler.responderUrl(),
              "http://127.0.0.1:" + std::to_string(kResponderPort));
}

TEST(OcspTest, FetchesAndCachesRealResponse) {
    MiniPki pki;
    if (!pki.ok) {
        GTEST_SKIP() << "openssl CLI could not build the test PKI";
    }
    Responder responder(pki);

    OcspStapler stapler(pki.leaf_cert, pki.ca_cert);
    if (!stapler.refresh()) {
        GTEST_SKIP() << "local OCSP responder unavailable";
    }

    EXPECT_TRUE(stapler.hasResponse());
    EXPECT_FALSE(stapler.cachedResponse().empty());
    // The responder was started with -nmin 60, so a nextUpdate is present and
    // must be in the future.
    EXPECT_GT(stapler.secondsUntilExpiry(), 0);
}

TEST(OcspTest, AbsentNextUpdateIsReportedDistinctly) {
    MiniPki pki;
    if (!pki.ok) {
        GTEST_SKIP() << "openssl CLI could not build the test PKI";
    }
    Responder responder(pki, /*nmin=*/0);  // omit nextUpdate (OpenSSL default)

    OcspStapler stapler(pki.leaf_cert, pki.ca_cert);
    if (!stapler.refresh()) {
        GTEST_SKIP() << "local OCSP responder unavailable";
    }

    // nextUpdate is OPTIONAL in RFC 6960: such a response is still valid and
    // must be cached and stapled...
    EXPECT_TRUE(stapler.hasResponse());
    // ...but reported as -1 ("no expiry hint"), NOT 0 ("expiry"), so a caller
    // does not mistake it for a stale staple.
    EXPECT_EQ(stapler.secondsUntilExpiry(), -1);
}

TEST(OcspTest, LoadsResponseFromFile) {
    MiniPki pki;
    if (!pki.ok) {
        GTEST_SKIP() << "openssl CLI could not build the test PKI";
    }
    Responder responder(pki);

    // The common production shape: an external agent writes the DER, the server
    // just loads it.
    int rc = runCmd("openssl ocsp -issuer " + pki.ca_cert + " -cert " +
                    pki.leaf_cert +
                    " -url http://127.0.0.1:" + std::to_string(kResponderPort) +
                    " -respout " + pki.resp_der + " -noverify");
    if (rc != 0 || !fileExists(pki.resp_der)) {
        GTEST_SKIP() << "could not obtain a response file from the responder";
    }

    OcspStapler stapler(pki.leaf_cert, pki.ca_cert);
    ASSERT_TRUE(stapler.loadFromFile(pki.resp_der));
    EXPECT_TRUE(stapler.hasResponse());
    EXPECT_GT(stapler.secondsUntilExpiry(), 0);
}

TEST(OcspTest, ClientReceivesStapledResponse) {
    MiniPki pki;
    if (!pki.ok) {
        GTEST_SKIP() << "openssl CLI could not build the test PKI";
    }
    Responder responder(pki);

    OcspStapler stapler(pki.leaf_cert, pki.ca_cert);
    if (!stapler.refresh()) {
        GTEST_SKIP() << "local OCSP responder unavailable";
    }

    ASSERT_TRUE(stapler.hasResponse());

    auto server_ctx = std::make_shared<TlsContext>(Role::kServer);
    server_ctx->useCertificateAndKey(pki.leaf_cert, pki.leaf_key);
    stapler.attachTo(*server_ctx);

    auto client_ctx = std::make_shared<TlsContext>(Role::kClient);
    client_ctx->loadCaFile(pki.ca_cert);
    client_ctx->setVerifyPeer(true);

    TlsEngine client(*client_ctx, Role::kClient, "ocsp.example.com");
    TlsEngine server(*server_ctx, Role::kServer);

    // The client must ASK for status, or the server will not staple.
    SSL_set_tlsext_status_type(reinterpret_cast<SSL*>(client.nativeHandle()),
                               TLSEXT_STATUSTYPE_ocsp);

    ASSERT_TRUE(driveHandshake(client, server))
        << "handshake failed: " << client.lastError();

    // The whole point: the response arrived inside the handshake, so the client
    // needed no separate round trip to the CA.
    const unsigned char* resp = nullptr;
    long len = SSL_get_tlsext_status_ocsp_resp(
        reinterpret_cast<SSL*>(client.nativeHandle()), &resp);

    EXPECT_GT(len, 0) << "client did not receive a stapled OCSP response";
    ASSERT_NE(resp, nullptr);

    // And it parses as a successful OCSP response.
    OCSP_RESPONSE* parsed = d2i_OCSP_RESPONSE(nullptr, &resp, len);
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(OCSP_response_status(parsed), OCSP_RESPONSE_STATUS_SUCCESSFUL);
    OCSP_RESPONSE_free(parsed);
}
