// Unit test for the TLS state machine (TlsEngine) with NO sockets: a server and
// a client engine drive a full handshake and exchange application data by
// piping each other's encrypted output back and forth in memory.

#include "tls_context.h"
#include "tls_engine.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using tls::Role;
using tls::TlsContext;
using tls::TlsEngine;

namespace
{

// Generate an ephemeral self-signed cert+key pair for the test, returning the
// two temp file paths. Uses the openssl CLI (available in the build env).
struct TestCert {
    std::string cert_path;
    std::string key_path;

    TestCert()
    {
        cert_path = "/tmp/cox_tls_test_cert.pem";
        key_path = "/tmp/cox_tls_test_key.pem";
        std::string cmd =
            "openssl req -x509 -newkey rsa:2048 -nodes "
            "-keyout " + key_path + " -out " + cert_path +
            " -days 1 -subj '/CN=localhost' >/dev/null 2>&1";
        // See note in sni_test.cc: valid() below is the real check.
        (void)test_util::runCmd(cmd);
    }
    ~TestCert()
    {
        std::remove(cert_path.c_str());
        std::remove(key_path.c_str());
    }
    bool valid() const {
        std::ifstream c(cert_path), k(key_path);
        return c.good() && k.good();
    }
};

// Pump the pending encrypted output of `from` into `to`. Returns bytes moved.
size_t pump(TlsEngine& from, TlsEngine& to)
{
    std::string enc;
    from.takeEncrypted(enc);
    if (!enc.empty())
    {
        to.feedEncrypted(enc.data(), enc.size());
    }
    return enc.size();
}

// Drive both engines' handshakes to completion by shuttling bytes until both
// report done (or a bounded number of rounds elapses).
bool driveHandshake(TlsEngine& client, TlsEngine& server)
{
    for (int round = 0; round < 20; ++round)
    {
        client.handshake();
        pump(client, server);
        server.handshake();
        pump(server, client);
        if (client.handshakeDone() && server.handshakeDone())
        {
            return true;
        }
    }
    return client.handshakeDone() && server.handshakeDone();
}

}   // namespace

TEST(TlsEngineTest, HandshakeCompletes)
{
    TestCert cert;
    ASSERT_TRUE(cert.valid()) << "failed to generate test cert";

    TlsContext server_ctx(Role::kServer);
    server_ctx.useCertificateAndKey(cert.cert_path, cert.key_path);

    TlsContext client_ctx(Role::kClient);
    // Test client does not verify (self-signed); exercise the handshake only.
    client_ctx.setVerifyPeer(false);

    TlsEngine server(server_ctx, Role::kServer);
    TlsEngine client(client_ctx, Role::kClient);

    ASSERT_TRUE(driveHandshake(client, server));
    EXPECT_TRUE(client.handshakeDone());
    EXPECT_TRUE(server.handshakeDone());

    // TLS 1.2 or 1.3 negotiated.
    EXPECT_FALSE(client.protocolVersion().empty());
    EXPECT_FALSE(client.cipherName().empty());
}

TEST(TlsEngineTest, EncryptedDataRoundTrip)
{
    TestCert cert;
    ASSERT_TRUE(cert.valid());

    TlsContext server_ctx(Role::kServer);
    server_ctx.useCertificateAndKey(cert.cert_path, cert.key_path);
    TlsContext client_ctx(Role::kClient);
    client_ctx.setVerifyPeer(false);

    TlsEngine server(server_ctx, Role::kServer);
    TlsEngine client(client_ctx, Role::kClient);
    ASSERT_TRUE(driveHandshake(client, server));

    // Client -> server.
    const std::string msg = "hello over TLS";
    size_t consumed = 0;
    ASSERT_EQ(client.writePlaintext(msg.data(), msg.size(), &consumed),
              TlsEngine::Status::kOk);
    EXPECT_EQ(consumed, msg.size());
    pump(client, server);

    std::string received;
    ASSERT_EQ(server.readPlaintext(received), TlsEngine::Status::kOk);
    EXPECT_EQ(received, msg);

    // Server -> client (reply).
    const std::string reply = "ack from server";
    ASSERT_EQ(server.writePlaintext(reply.data(), reply.size(), &consumed),
              TlsEngine::Status::kOk);
    pump(server, client);

    std::string got;
    ASSERT_EQ(client.readPlaintext(got), TlsEngine::Status::kOk);
    EXPECT_EQ(got, reply);
}

TEST(TlsEngineTest, LargeDataRoundTrip)
{
    TestCert cert;
    ASSERT_TRUE(cert.valid());

    TlsContext server_ctx(Role::kServer);
    server_ctx.useCertificateAndKey(cert.cert_path, cert.key_path);
    TlsContext client_ctx(Role::kClient);
    client_ctx.setVerifyPeer(false);

    TlsEngine server(server_ctx, Role::kServer);
    TlsEngine client(client_ctx, Role::kClient);
    ASSERT_TRUE(driveHandshake(client, server));

    // A payload larger than a single TLS record (16 KB) to exercise chunking.
    std::string big(100000, 'z');
    size_t offset = 0;
    std::string received;
    while (offset < big.size())
    {
        size_t consumed = 0;
        auto st = client.writePlaintext(big.data() + offset,
                                        big.size() - offset, &consumed);
        ASSERT_EQ(st, TlsEngine::Status::kOk);
        offset += consumed;
        pump(client, server);
        std::string chunk;
        ASSERT_EQ(server.readPlaintext(chunk), TlsEngine::Status::kOk);
        received += chunk;
    }

    // Drain any remaining buffered plaintext.
    pump(client, server);
    std::string tail;
    server.readPlaintext(tail);
    received += tail;

    EXPECT_EQ(received.size(), big.size());
    EXPECT_EQ(received, big);
}

TEST(TlsEngineTest, VerificationFailsWithoutTrust)
{
    TestCert cert;
    ASSERT_TRUE(cert.valid());

    TlsContext server_ctx(Role::kServer);
    server_ctx.useCertificateAndKey(cert.cert_path, cert.key_path);
    TlsContext client_ctx(Role::kClient);
    client_ctx.setVerifyPeer(true);  // verify, but no CA loaded for self-signed

    TlsEngine server(server_ctx, Role::kServer);
    TlsEngine client(client_ctx, Role::kClient);

    // Handshake must NOT complete successfully: the client rejects the
    // untrusted self-signed certificate.
    bool done = driveHandshake(client, server);
    EXPECT_FALSE(done);

}
