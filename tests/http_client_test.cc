// HttpClient tests: response-parser unit tests + end-to-end HttpClient <->
// HttpServer (both using Cox).

#include "http_client.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"
#include "response_parser.h"

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_connection.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace http;

namespace {
buffer::Buffer bufOf(const std::string& s) {
    buffer::Buffer b;
    b.append(s);
    return b;
}
}  // namespace

// ---- Response parser unit tests ----

TEST(ResponseParserTest, ContentLengthBody) {
    ResponseParser p;
    auto buf = bufOf(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: "
        "text/plain\r\n\r\n"
        "hello");
    ASSERT_TRUE(p.parse(&buf));
    ASSERT_TRUE(p.gotAll());
    EXPECT_EQ(p.response().statusCode(), 200);
    EXPECT_EQ(p.response().statusReason(), "OK");
    EXPECT_EQ(p.response().body(), "hello");
    EXPECT_EQ(p.response().header("Content-Type"), "text/plain");
}

TEST(ResponseParserTest, ChunkedBody) {
    ResponseParser p;
    auto buf = bufOf(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n");
    ASSERT_TRUE(p.parse(&buf));
    ASSERT_TRUE(p.gotAll());
    EXPECT_EQ(p.response().body(), "abcdef");
}

TEST(ResponseParserTest, EofDelimitedBody) {
    ResponseParser p;
    // No Content-Length, no chunked -> body until EOF.
    auto buf = bufOf("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nsome data");
    ASSERT_TRUE(p.parse(&buf));
    EXPECT_FALSE(p.gotAll());  // not complete until EOF
    p.onEof();
    EXPECT_TRUE(p.gotAll());
    EXPECT_EQ(p.response().body(), "some data");
}

TEST(ResponseParserTest, StatusCodeParsed) {
    ResponseParser p;
    auto buf = bufOf("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    ASSERT_TRUE(p.parse(&buf));
    ASSERT_TRUE(p.gotAll());
    EXPECT_EQ(p.response().statusCode(), 404);
    EXPECT_EQ(p.response().statusReason(), "Not Found");
}

// ---- End-to-end client <-> server ----
TEST(HttpClientTest, GetAgainstCoxServer) {
    const uint16_t kPort = 55601;
    std::atomic<bool> srv_started{false};
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;
        HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort), "srv");
        server.setHttpHandler([](const HttpRequest& req, HttpResponse* resp) {
            resp->setStatus(200);
            resp->setContentType("text/plain");
            resp->setBody("hello " + req.path());
        });

        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    std::atomic<bool> cli_started{false};
    std::atomic<bool> got{false};
    int status = 0;
    std::string body;
    reactor::EventLoop* cli_loop = nullptr;

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        cli_loop = &loop;
        auto client = std::make_shared<HttpClient>(
            &loop, net::InetAddress("127.0.0.1", kPort), "localhost");
        client->get("/world",
                    [&](const HttpResponse& resp, const std::string& err) {
                        if (err.empty()) {
                            status = resp.statusCode();
                            body = resp.body();
                        }
                        got = true;
                        loop.quit();
                    });
        cli_started = true;
        loop.loop();
    });

    while (!cli_started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 200 && !got.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(got.load());
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, "hello /world");

    // The client callback already called loop.quit(); just join. (Calling
    // cli_loop->quit() from here would touch a loop whose thread has exited.)
    client_thread.join();
    srv_loop->quit();
    server_thread.join();
}

TEST(HttpClientTest, PostAgainstCoxServer) {
    const uint16_t kPort = 55602;
    std::atomic<bool> srv_started{false};
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;
        HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort), "srv");
        server.setHttpHandler([](const HttpRequest& req, HttpResponse* resp) {
            resp->setStatus(200);
            resp->setBody("echo:" + req.body());
        });

        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    std::atomic<bool> got{false};
    std::string body;
    reactor::EventLoop* cli_loop = nullptr;
    std::atomic<bool> cli_started{false};

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        cli_loop = &loop;
        auto client = std::make_shared<HttpClient>(
            &loop, net::InetAddress("127.0.0.1", kPort), "localhost");
        HttpRequest req;
        req.setMethod(Method::kPost);
        req.setPath("/submit");
        req.setBody("payload123");
        client->send(req,
                     [&](const HttpResponse& resp, const std::string& err) {
                         if (err.empty()) {
                             body = resp.body();
                         }
                         got = true;
                         loop.quit();
                     });
        cli_started = true;
        loop.loop();
    });

    while (!cli_started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 200 && !got.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(got.load());
    EXPECT_EQ(body, "echo:payload123");

    // Client callback already quit the loop; just join.
    client_thread.join();
    srv_loop->quit();
    server_thread.join();
}

TEST(HttpClientTest, KeepAliveReusesConnection) {
    const uint16_t kPort = 55603;
    std::atomic<bool> srv_started{false};
    std::atomic<int> server_reqs{0};   // requests the server handled
    std::atomic<int> server_conns{0};  // Connections accepted (via observer)
    reactor::EventLoop* srv_loop = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;
        HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort), "srv");
        // Observe connections WITHOUT hijacking HttpServer's internal callback.
        server.setConnectionObserver([&](const net::TcpConnectionPtr& c) {
            if (c->connected()) server_conns++;
        });
        server.setHttpHandler([&](const HttpRequest& req, HttpResponse* resp) {
            server_reqs++;
            resp->setStatus(200);
            resp->setBody("r:" + req.path());
        });

        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    std::atomic<int> replies{0};
    std::string body1, body2;
    reactor::EventLoop* cli_loop = nullptr;
    std::atomic<bool> cli_started{false};

    std::thread client_thread([&] {
        reactor::EventLoop loop;
        cli_loop = &loop;
        auto client = std::make_shared<HttpClient>(
            &loop, net::InetAddress("127.0.0.1", kPort), "localhost");
        // HttpClient now keeps the connection alive by default (single-conn
        // pool).

        // First request; in its callback, issue the second (sequential reuse).
        client->get("/a", [&, client](const HttpResponse& resp,
                                      const std::string& err) {
            if (err.empty()) {
                body1 = resp.body();
            }
            replies++;

            client->get(
                "/b", [&](const HttpResponse& resp2, const std::string& err2) {
                    if (err2.empty()) body2 = resp2.body();
                    replies++;
                    loop.quit();
                });
        });
        cli_started = true;
        loop.loop();
    });

    while (!cli_started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 300 && replies.load() < 2; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(replies.load(), 2);
    EXPECT_EQ(body1, "r:/a");
    EXPECT_EQ(body2, "r:/b");
    EXPECT_EQ(server_reqs.load(), 2);   // both requests handled
    EXPECT_EQ(server_conns.load(), 1);  // over a single reused connection

    client_thread.join();
    srv_loop->quit();
    server_thread.join();
}
