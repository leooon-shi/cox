// A minimal TCP echo server built on the Cox net library.
//
// Usage: echo_server [port]    (default port 9000)
//
// Connect with: nc 127.0.0.1 9000
// Everything you type is echoed back. Idle connections are closed after 60s.

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using net::InetAddress;
using net::TcpConnectionPtr;
using net::TcpServer;

namespace {
// Set by the signal handler; polled by the event loop to trigger graceful
// shutdown. sig_automic_t + volatile is the async-signal-safe idiom.
volatile std::sig_atomic_t g_shutdown_requested = 0;
void onSignal(int) { g_shutdown_requested = 1; }
}   // namespace

int main(int argc, char* argv[]) {
    // Unix-domain mode: echo_server -- unix /path/to/socket
    if (argc > 2 && std::string(argv[1]) == "--unix") {
        reactor::EventLoop loop;
        InetAddress listen_addr = InetAddress::unixDomain(argv[2]);
        TcpServer server(&loop, listen_addr, "echo-uds");
        server.setIdleTimeout(60);
        server.setMaxConnections(10000);
        server.setConnectionCallback([](const TcpConnectionPtr& conn) {
            std::cout << "connection " << conn->name()
                      << (conn->connected() ? "UP" : "DOWN") << std::endl;
        });
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
                conn->send(buf->retrieveAllAsString());
            });
        std::cout << "echo server listening on" << listen_addr.toIpPort()
                  << std::endl;
        server.start();
        loop.loop();
        return 0;
    }

    uint16_t port = 9000;
    if (argc > 1)
    {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    reactor::EventLoop loop;
    InetAddress listen_addr(port);
    TcpServer server(&loop, listen_addr, "echo");
    server.setIdleTimeout(60);
    server.setMaxConnections(10000);
    server.setConnectionCallback([](const TcpConnectionPtr& conn) {
        std::cout << "connection " << conn->name() << " from "
                  << conn->peerAddress().toIpPort()
                  << (conn->connected() ? "UP" : "DOWN") << std::endl;
    });
    server.setMessageCallback(
        [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
            std::string msg = buf->retrieveAllAsString();
            conn->send(msg); // echo it back
        });
    std::cout << "echo server listening on" << listen_addr.toIpPort()
                << std::endl;
    server.start();
    // Install SIGINT/SIGTERM handlers for graceful shutdown. The handler only
    // sets a flag; a periodic timer polls it and initiates the drain from the
    // loop thread (safe to call the full API there).
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    bool draining = false;
    loop.runEvery(100, [&]() {
        if (g_shutdown_requested && !draining) {
            draining = true;
            std::cout << "\nsignal received; draining (30s deadline)..."
                      << std::endl;
            server.shutdown(30, [&loop]() {
                std::cout << "drain complete; exiting" << std::endl;
                loop.quit();
            });
        }
    });
    loop.loop();
    return 0;
}