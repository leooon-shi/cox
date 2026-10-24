// End-to-end HTTPS: Cox Httpserver (TLS-terminating) <-> Cox HttpClient (TLS),
// plus DNS resolver tests live separately.

#include "http_client.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"

#include "event_loop.h"
#include "inet_address.h"
#include "tls_context.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace http;

namespace
{
struct TestCert{
    std::string cert = "/tmp/cox_https_cert.pem";
    std::string key = "/tmp/cox_https_key.pem";
    TestCert() {
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout " +
                        key + " -out" + cert +
                        " -days 1 -subj '/CN=localhost' >/dev/null 2>&1";
        // See note in sni_test.cc: the valid() probe below is the real check.
        (void)test_util::runCmd(cmd);
    }
    ~TestCert() { std::remove(cert.c_str()); std::remove(key.c_str()); }

    bool valid() const { std::ifstream c(cert), k(key); return c.good() && k.good(); }
};

} // namespace

TEST(HttpsTest, ServerAndClientEndToEnd)
{
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 55901;

    std::atomic<bool> srv_started{false};
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&]{
        reactor::EventLoop loop;
        loop_ptr = &loop;
        auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kServer);
        ctx->useCertificateAndKey(cert.cert, cert.key);

        HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort), "https", ctx);
        server.setHttpHandler([](const HttpRequest& req, HttpResponse* resp){
            resp->setStatus(200);
            resp->setContentType("text/plain");
            resp->setBody("secure:" + req.path());
        });
        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    std::atomic<bool> got {false};
    int status = 0;
    std::string body;
    std::atomic<bool> cli_started {false};

    auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kClient);
    ctx->setVerifyPeer(false);          // self-signed test cert

    std::thread client_thread([&]{
        reactor::EventLoop loop;
        cli_loop = &loop;
        auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kClient);
        ctx->setVerifyPeer(false);              // self-signed test cert
        auto client = std::make_shared<HttpClient>(
            &loop, net::InetAddress("127.0.0.1", kPort), ctx, "localhost");
        client->get("/data", [&](const HttpResponse& resp, const std::string& err){
            if (err.empty())
            {
                status = resp.statusCode(); body = resp.body();
            }
            got = true;
            loop.quit();
        });
        cli_started = true;
        loop.loop();
    });

    while (!cli_started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 300 && !got.load(); ++i) std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(got.load());
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, "secure:/data");

    client_thread.join();
    srv_loop->quit();
    server_thread.join();
}