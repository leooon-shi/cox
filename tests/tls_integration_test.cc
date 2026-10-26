// End-to-end TLS over real sockets: a TcpServer wraps each accepted connection
// in a server-side TlsConnection; a TcpClient wraps its connection in a
// client-side TlsConnection. Full handshake + encrypted echo round-trip.

#include "tls_connection.h"
#include "tls_context.h"

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_client.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using net::InetAddress;
using net::TcpClient;
using net::TcpConnectionPtr;
using net::TcpServer;
using tls::Role;
using tls::TlsConnection;
using tls::TlsConnectionPtr;
using tls::TlsContext;
using tls::TlsContextPtr;

namespace {
struct TestCert {
    std::string cert_path = "/tmp/cox_tls_e2e_cert.pem";
    std::string key_path = "/tmp/cox_tls_e2e_key.pem";
    TestCert() {
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout " +
                          key_path + " -out " + cert_path +
                          " -days 1 -subj '/CN=localhost' "
                          ">/dev/null 2>&1";
        // See note in sni_test.cc: valid() below is the real check.
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

}  // namespace

TEST(TlsIntegrationTest, HandshakeAndEncryptedEcho) {
    TestCert cert;
    ASSERT_TRUE(cert.valid());

    const uint16_t kPort = 52001;
    std::atomic<bool> srv_started{false};
    std::atomic<bool> cli_started{false};
    std::atomic<bool> got_reply{false};
    std::string reply;
    reactor::EventLoop* srv_loop = nullptr;
    reactor::EventLoop* cli_loop = nullptr;

    // Keep server-side TlsConnections alive for the connection's lifetime.
    std::mutex srv_mutex;
    std::map<std::string, TlsConnectionPtr> srv_tls;

    // ---- Server thread ----
    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;

        auto ctx = std::make_shared<TlsContext>(Role::kServer);
        ctx->useCertificateAndKey(cert.cert_path, cert.key_path);

        InetAddress addr("127.0.0.1", kPort);
        TcpServer server(&loop, addr, "tls-srv");
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                auto tls_conn =
                    std::make_shared<TlsConnection>(conn, ctx, Role::kServer);
                tls_conn->setMessageCallback(
                    [](const TlsConnectionPtr& tc, std::string& msg) {
                        tc->send(msg);  // echo decrypted plaintext back
                    });
                {
                    std::lock_guard<std::mutex> lock(srv_mutex);
                    srv_tls[conn->name()] = tls_conn;
                }
                tls_conn->start();
            } else {
                std::lock_guard<std::mutex> lock(srv_mutex);
                srv_tls.erase(conn->name());
            }
        });

        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // ---- Client thread ----
    TlsConnectionPtr client_tls;
    std::mutex cli_mutex;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        cli_loop = &loop;

        auto ctx = std::make_shared<TlsContext>(Role::kClient);
        ctx->setVerifyPeer(false);  // self-signed test cert

        InetAddress addr("127.0.0.1", kPort);
        TcpClient client(&loop, addr, "tls-cli");
        client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                auto tc =
                    std::make_shared<TlsConnection>(conn, ctx, Role::kClient);
                tc->setHandshakeDoneCallback([](const TlsConnectionPtr& t) {
                    t->send("secret hello");  // send once secure
                });
                tc->setMessageCallback(
                    [&](const TlsConnectionPtr&, std::string& msg) {
                        reply = msg;
                        got_reply = true;
                    });
                {
                    std::lock_guard<std::mutex> lock(cli_mutex);
                    client_tls = tc;
                }
                tc->start();
            }
        });
        client.connect();
        cli_started = true;
        loop.loop();
    });

    while (!cli_started.load()) std::this_thread::sleep_for(1ms);

    for (int i = 0; i < 300 && !got_reply.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_TRUE(got_reply.load());
    EXPECT_EQ(reply, "secret hello");
    {
        std::lock_guard<std::mutex> lock(cli_mutex);
        if (client_tls) {
            EXPECT_TRUE(client_tls->handshakeDone());
            EXPECT_FALSE(client_tls->protocolVersion().empty());
        }
    }

    {
        std::lock_guard<std::mutex> lock(cli_mutex);
        client_tls.reset();
    }

    cli_loop->quit();
    client_thread.join();
    srv_loop->quit();
    server_thread.join();
}
