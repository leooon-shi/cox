// End-to-end HTTP/1.1 server test over real sockets, using a raw client.

#include "http_server.h"
#include "http_request.h"
#include "http_response.h"

#include "event_loop.h"
#include "inet_address.h"
#include "socket.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace http;
namespace sockets = net::sockets;

namespace {
bool waitReadable(int fd, int ms) {
    pollfd pfd{fd, POLLIN, 0};
    return ::poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLIN);
}
bool waitWritable(int fd, int ms) {
    pollfd pfd{fd, POLLOUT, 0};
    return ::poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLOUT);
}
int connectClient(uint16_t port) {
    int fd = sockets::createNonBlockingOrDie(AF_INET);
    net::InetAddress server("127.0.0.1", port);
    sockets::connect(fd, server);
    waitWritable(fd, 1000);
    return fd;
}
std::string readAll(int fd, int ms) {
    std::string out;
    char buf[4096];
    // Read until we have headers + body or timeout.
    for (int i = 0; i < 20; ++i) {
        if (!waitReadable(fd, ms)) break;
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, n);
        if (out.find("\r\n\r\n") != std::string::npos) {
            // crude: assume small body arrives together
            std::this_thread::sleep_for(10ms);
            while (waitReadable(fd, 10)) {
                n = ::read(fd, buf, sizeof(buf));
                if (n <= 0) break;
                out.append(buf, n);
            }
            break;
        }
    }
    return out;
}
}  // namespace

TEST(HttpServerTest, GetRequestResponse) {
    const uint16_t kPort = 55501;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort), "http");
        server.setHttpHandler([](const HttpRequest& req, HttpResponse* resp) {
            resp->setStatus(200);
            resp->setContentType("text/plain");
            resp->setBody("path=" + req.path());
        });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    int client = connectClient(kPort);
    std::string req = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_TRUE(test_util::sendAll(client, req));

    std::string resp = readAll(client, 1000);

    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp.find("path=/hello"), std::string::npos);

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}

TEST(HttpServerTest, KeepAliveTwoRequests) {
    const uint16_t kPort = 55502;
    std::atomic<bool> started{false};
    std::atomic<int> request_count{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort), "http");
        server.setHttpHandler([&](const HttpRequest&, HttpResponse* resp) {
            request_count++;
            resp->setStatus(200);
            resp->setBody("ok");
        });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    int client = connectClient(kPort);
    // First request (keep-alive default in 1.1).
    std::string r1 = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_TRUE(test_util::sendAll(client, r1));
    std::string resp1 = readAll(client, 1000);
    EXPECT_NE(resp1.find("200 OK"), std::string::npos);

    // Second request on the SAME connection.
    std::string r2 = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_TRUE(test_util::sendAll(client, r2));
    std::string resp2 = readAll(client, 1000);
    EXPECT_NE(resp2.find("200 OK"), std::string::npos);

    EXPECT_EQ(request_count.load(), 2);

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}

TEST(HttpServerTest, PostEcho) {
    const uint16_t kPort = 55503;
    std::atomic<bool> started{false};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread server_thread([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        HttpServer server(&loop, net::InetAddress("127.0.0.1", kPort), "http");
        server.setHttpHandler([](const HttpRequest& req, HttpResponse* resp) {
            resp->setStatus(200);
            resp->setBody(req.body());  // echo the posted body
        });
        server.start();
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);

    int client = connectClient(kPort);
    std::string body = "the payload";
    std::string req = "POST /submit HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\n\r\n" + body;
    ASSERT_TRUE(test_util::sendAll(client, req));

    std::string resp = readAll(client, 1000);
    EXPECT_NE(resp.find("the payload"), std::string::npos);

    ::close(client);
    loop_ptr->quit();
    server_thread.join();
}
