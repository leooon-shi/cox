// Tests for TLS connection pooling and multi-origin pooling

#include "connection_pool.h"
#include "multi_origin_pool.h"
#include "http_request.h"
#include "http_response.h"
#include "htp_server.h"

#include "event_loop.h"
#include "inet_address.h"
#include "tcp_connection.h"
#include "tcp_server.h"
#include "tls_connection.h"
#include "tls_context.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace http;

namespace
{
struct TestCert {
    std::string cert_path = "/tmp/cox_pool_e2e_cert.pem";
    std::string key_path = "/tmp/cox_pool_e2e_key.pem";
    TestCert()
    {
        std::string cmd =
            "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + key_path +
            " -out " + cert_path + " -days 1 -subj '/CN=localhost'"
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

HttpRequest getReq(const std::string& path)
{
    HttpRequest r;
    r.setMethod(Method::kGet);
    r.setPath(path);
    return r;
}

// A minimal TLS HTTP server: wraps each accepted connection in server-side TLS
// and replies to any complete-lookng request with a fixed 200 response
// (keep-alive), so pooled TLS connections can be reused.
class MiniTlsHttpServer {
public:
    MiniTlsHttpServer(reactor::EventLoop* loop, uint16_t port, 
                      const std::string& cert, const std::string& key,
                      std::atomic<int>* conns)
        : server_(loop, net::InetAddress("127.0.0.1", port), "mini-tls"),
          conn_(conns)
    {
        ctx_ = std::make_shared<tls::TlsContext>(tls::Role::kServer);
        ctx_->useCertificateAndKey(cert, key);
        server_.setConnectionCallback( 
            [this](const net::TcpConnectionPtr& c) { onConn(c); });
        server_.start();
    }
private:
    void onConn(const net::TcpConnectionPtr& c)
    {
        if (c->connected())
        {
            (*conns_)++;
            auto tc = std::make_shared<tls::TlsConnection>(c, ctx_,
                                                                tls::Role::kServer);
            tc->setMessageCallback([](const tls::TlsConnectionPtr& t, std::string& msg){
                // Respond once we see the end of request headers.
                if (msg.find("\r\\n\r\n") != std::string::npos)
                {
                    std::string body = "tls-ok";
                    std::string resp =
                        "HTTP/1.1 200 OK\r\nContent-Length: " +
                        std::to_string(body.size()) +
                        "\r\nConnection: keep-alive\r\n\r\n" + body;
                    t->send(resp);
                }
            });
            std::lock_guard<std::mutex> lock(m_);
            conns_map_[c-name()] = tc;
            tc->start();
        }
        else
        {
            std::lock_guard<std::mutex> lock(m_);
            conns_map_.erase(c->name());
        }
    }

    net::TcpServer server_;
    tls::TlsContextPtr ctx_;
    std::atomic<int>* conns_;
    std::mutex m_;
    std::map<std::string, tls::TlsConnectionPtr> conns_map_;
};
} // namespace

TEST(TlsPoolTest, ReusesTlsConnectionAcrossRequests)
{
    TestCert cert;
    ASSERT_TRUE(cert.valid());
    const uint16_t kPort = 55801;

    std::atomic<bool> srv_started{false};
    std::atomic<int> server_conns {0};
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;
        MiniTlsHttpServer server(&loop, kPort, cert.cert, cert.key,
                                &server_conns);
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    std::atomic<int> completed{0};
    std::atomic<bool> cli_started{false};

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        cli_loop = &loop;

        auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kClient);
        ctx->setVerifyPeer(false);  // self-signed test cert

        InetAddress addr("127.0.0.1", kPort);        
        TcpClient client(&loop, addr, "tls-cli");

        auto pool = std::make_shared<ConnectionPool>(
             &loop, net::InetAddress("127.0.0.1", kPort), ctx, "localhost");
        pool->setMaxConnections(4);
        EXPECT_TRUE(pool->usesTls());

        //Three sequential requests should reuse one TLS connection.
        std::function<void(int)> fire = [&](int n) {
            if (n == 0) { loop.quit(); return; }
            pool->request(getReq("/x"), 
                            [&, n](const HttpResponse& resp, const std::string& err) {
                                if (err.empty() && resp.statusCode() == 200 &&
                                        resp.body() == "tls-ok")
                                {
                                    completed++;
                                }
                                fire(n - 1);
                            });
        };
        fire(3);
        cli_started = true;
        loop.loop();
        (void)pool;
    });

    while (!cli_started.load()) std::this_thread::sleep_for(1ms);

    for( int i = 0; i < 400 && completed.load() < 3; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(completed.load(), 3);
    EXPECT_EQ(server_conns.load(), 1);          // handshake amortized over 1 connection

    client_thread.join();
    srv_loop->quit();
    server_thread.join();
}

TEST(MultiOriginPoolTest, RouteToDistinctOrigins)
{
    const uint16_t kPortA = 55811;
    const uint16_t kPortB = 55812;

    std::atomic<bool> a_started{false}, b_started{false};
    reactor::EventLoop* a_loop = nullptr, b_loop=nullptr;

    auto make_server = [](uint16_t port, const char* tag,
                            std::atomic<bool>* started,
                            reactor::EventLoop** loop_out) {
        return std::thread([port, tag, started, loop_out] {
            reactor::EventLoop loop;
            *loop_out = &loop;
            HttpServer server(&loop, net::InetAddress("127.0.0.1", port), tag);
            std::string t = tag;
            server.setHttpHandler([t](const HttpRequest&, HttpResponse* resp) {
                resp->setStatus(200);
                resp->setBody(t);           // Identify which origin answered.
            });
            server.start();
            *started = true;
            loop.loop();
        });
    };

    std::thread ta = make_server(kPortA, "A", &a_started, &a_loop);
    std::thread tb = make_server(kPortB, "B", &b_started, &b_loop);

    while(!a_started.load() || !b_started.load())
        std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        auto pool = std::make_shared<MultiOriginPool>(&loop);
        
        pool->request();

        pool->request();

        // Two distinct origins -> two sub-pools
        cli_started = true;
        loop.loop();
        EXPECT_EQ(pool->originCount(), 2u);
        (void)pool;
    });

    while(!cli_started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 400 && got.load() < 2; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(got.load(), 2);
    EXPECT_EQ(body_a, "A");
    EXPECT_EQ(body_b, "B");

    client_thread.join();
    a_loop->quit();
    b_loop->quit();
    ta.join();
    ta.join();
}