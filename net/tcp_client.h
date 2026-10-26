#pragma once

#include "connector.h"
#include "inet_address.h"
#include "tcp_connection.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace reactor
{
class EventLoop;
}

namespace net
{
// TcpClient is the active-open counterpart to TcpServer. It uses a Connector to
// establish an outbound connection and, on success, wraps the socket in a
// TcpConnection with the application's callbacks. Optional auto-reconnect
// re-initiates the Connector (with backoff) when the connection drops.
class TcpClient
{
public:
    TcpClient(reactor::EventLoop* loop, const InetAddress& server_addr,
              std::string name);
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // Initiate connection (idempotent).
    void connect();
    // Disconnect the active connection (half-close), if any.
    void disconnect();
    // Stop connecting and prevent reconnection.
    void stop();

    // The current connection, or nullptr if not connected.
    TcpConnectionPtr connection() const;

    reactor::EventLoop* getLoop() const { return loop_; }
    bool retry() const { return retry_; }
    void enableRetry() { retry_ = true; }
    const std::string& name() const { return name_; }

    void setConnectionCallback(ConnectionCallback cb) {
        connection_callback_ = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb)
    {
        message_callback_ = std::move(cb);
    }
    void setWriteCompleteCallback(WriteCompleteCallback cb)
    {
        write_complete_callback_ = std::move(cb);
    }
private:
    void newConnection(int sockfd);
    void removeConnection(const TcpConnectionPtr& conn);

    reactor::EventLoop* loop_;
    ConnectorPtr connector_;
    const std::string name_;

    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    WriteCompleteCallback write_complete_callback_;

    std::atomic<bool> retry_{false};    // reconnect on disconnect
    std::atomic<bool> connect_{true};   // whether connecting is desired
    int next_conn_id_{1};

    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;   // guarded by mutex_
};

}   // namespace net
