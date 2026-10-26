#pragma once

#include "io_channel.h"
#include "socket.h"

#include <functional>

namespace reactor {
class EventLoop;
}

namespace net
{

class InetAddress;
// Acceptor is the passive-open side of the Acceptor-Connector pattern. It owns
// a listening socket registered with an EventLoop and, on each readable event,
// accepts pending connections and hands each new connection fd (plus the peer
// address) to a user-supplied callback.
//
// The Acceptor performs no I/O on accepted connections itself; wiring an
// accepted fd into a TcpConnection is the responsibility of the callback
// (typically TcpServer).
class Acceptor
{
public:
    // Called with (connfd, peer_address) for each accepted connection. The
    // callback takes ownership of connfd.
    using NewConnectionCallback =
        std::function<void(int sockfd, const InetAddress& peer)>;

    Acceptor(reactor::EventLoop* loop, const InetAddress& listen_addr,
             bool reuse_port);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void setNewConnectionCallback(NewConnectionCallback cb)
    {
        new_connection_callback_ = std::move(cb);
    }

    bool listening() const { return listening_; }
    void listen();
    // Stop accepting new connections (disable the listen socket's read events).
    // The listening socket stays open but idle; used during graceful drain.
    void stopListening();

private:
    void handleRead();

    reactor::EventLoop* loop_;
    Socket accept_socket_;
    reactor::Channel accept_channel_;
    NewConnectionCallback new_connection_callback_;
    bool listening_;
    // A spare fd held open so we can gracefully accept+close a pending
    // connection when the process hits EMFILE, avoiding a busy-loop.
    int idle_fd_;
};

}   // namespace net
