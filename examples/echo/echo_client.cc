// A minimal TCP echo client built on the Cox net library.
//
// Usage: echo_client [host] [port]   (default 127.0.0.1 9000)
//
// Sends a greeting on connect and prints whatever the server echoes back,
// then closes after the first reply.

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "tcp_client.h"
#include "tcp_connection.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using net::InetAddress;
using net::TcpClient;
using net::TcpConnectionPtr;

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::atoi(argv[2]));
    }

    reactor::EventLoop loop;
    InetAddress server_addr{host, port};
    TcpClient client(&loop, server_addr, "echo-client");
    client.enableRetry();

    client.setConnectionCallback([](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            std::cout << "connected to " << conn->peerAddress().toIpPort()
                      << std::endl;
            conn->send("hello from cox echo client");
        }
    });

    client.setMessageCallback(
        [&loop](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
            std::cout << "server echoed: " << buf->retrieveAllAsString()
                      << std::endl;
            conn->shutdown();
            loop.quit();
        });

    std::cout << "connecting to " << server_addr.toIpPort() << std::endl;
    client.connect();
    loop.loop();
    return 0;
}
