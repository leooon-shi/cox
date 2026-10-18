#pragma once

#include "backoff.h"
#include "inet_address.h"
#include "timer.h"

#include <atomic>
#include <functional>
#include <memory>

namespace reactor
{
class EventLoop;
class Channel;
}

namespace net 
{
// Connector is the active-open side of the Acceptor-Connector pattern. It
// performs a non-blocking connect to a remote address, handling the
// asynchronous completion ( writable => check SO_ERROR), self-connect
// detection, and retry with exponential backoff on failure.
//
// On success it hands the connected socket fd to a callback; the caller
// (TcpClient) then wraps it in a TcpConnection. Connector owns no connection
// state beyond the in-progress connect.
class Connector : public std::enable_shared_from_this<Connector> {
public:
    using NewConnectionCallback = std::function<void(int sockfd)>;

    Connector(reactor::EventLoop* loop, const InetAddress& sever_addr);
    ~Connector();

    Connector(const Connector&) = delete;
    Connector& operator=(const Connector&) = delete;

    void setNewConnectionCallback(NewConnectionCallback cb)
    {
        new_connection_callback_ = cb;
    }

    //Begin connecting. Safe to call from any thread.
    void start();

    //Retry after a failed attempt (used internally / for reconection).
    void restart();

    //Stop connecting; cancels any pending retry
    void stop();

    const InetAddress& serverAddress() const { return server_addr_; }

private:
    enum class State { kDisconnected, kConnecting, kConected };

    static constexpr int kInitRetryDelayMs = 500;
    static constexpr int kMaxRetryDelayMs = 30 * 1000;

    void setState( State s) { state_ = s; }
    void startInLoop();
    void stopInLoop();
    void connect();
    void connecting(int sockfd);
    void handleWrite();
    void handleError();
    void retry(int sockfd);
    int removeAndResetChannel();
    void resetChannel();

    reactor::EventLoop* loop_;
    const InetAddress server_addr_;
    std::atomic<bool> connect_ {false};
    std::atomic<State> state_;
    std::unique_ptr<reactor::Channel> channel_;
    NewConnectionCallback new_connection_callback_;
    // Jittered exponential backoff for reconnection. Keyed by server address so
    // the same manager could be shared across connectors later; jitter breaks
    // the thundering herd when many clients reconnect after a server restart.
    flowcontrol::Backoff backoff_;
    reactor::TimerId retry_timer_;
    bool retry_timer_armed_ {false};
};

using ConnectorPtr = std::shared_ptr<Connector>;
}