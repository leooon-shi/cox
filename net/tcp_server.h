#pragma once

#include "inet_address.h"
#include "tcp_connection.h"
#include "token_bucket.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace reactor
{
class EventLoop;
class EventLoopThreadPool;
}

namespace net
{
class Acceptor;

// TcpServer accepts incoming TCP connections and manages their lifecycle. It
// owns an Acceptor (on the base/acceptor loop) and a map of active
// TcpConnections. Application code sets connection/message/write-complete
// callbacks, which are propagated to each new connection.
//
// Concurrency: by default (thread num 0) all connections run on the base loop.
// Call setThreadNum(N) to distribute accepted connections round-robin across an
// EventLoopThreadPool of N io threads ("one loop per thread"). The acceptor
// always runs on the base loop; each connection is established on its assigned
// io loop.
class TcpServer
{
public:
    enum class Option { kNoReusePort, kReusePort };
    using ThreadInitCallback = std::function<void(reactor::EventLoop*)>;

    TcpServer(reactor::EventLoop* loop, const InetAddress& listen_addr,
              std::string name, Option option = Option::kNoReusePort);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    const std::string& name() const { return name_; }
    reactor::EventLoop* getLoop() const { return loop_; }

    // Number of io threads for the connection pool. 0 (default) => single loop.
    // Must be called before start().
    void setThreadNum(int num_threads);

    // Start listening (and start the io thread pool). Idempotent.
    void start();

    // Callback invoked (on the base loop) when a graceful shutdown has fully
    // drained: the acceptor is stopped and all connections are closed.
    using DrainCompleteCallback = std::function<void()>;

    // Begin a graceful shutdown / drain:
    // 1. stop accepting new connections,
    // 2. ask each live connection to finish (optional onDrain callback) and
    //    half-close so peers observe EOF once output drains,
    // 3. wait for connections to close on their own,
    // 4. after drain_deadline_seconds, force-close any stragglers,
    // 5. invoke on_complete when the last connection is gone.
    // Safe to call from any thread. Idempotent.
    void shutdown(uint32_t drain_deadline_seconds = 30,
                  DrainCompleteCallback on_complete = DrainCompleteCallback());
    // Optional per-connection callback invoked at drain start (on the
    // connection's io loop) so the application can initiate an orderly close
    // (e.g. send a final message, disable keep-alive) before half-close.
    void setDrainCallback(ConnectionCallback cb)
    {
        drain_callback_ = std::move(cb);
    }

    bool isDraining() const { return draining_; }

    void setThreadInitCallback(ThreadInitCallback cb)
    {
        thread_init_callback_ = std::move(cb);
    }

    void setConnectionCallback(ConnectionCallback cb)
    {
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

    // Apply an idle timeout (seconds) to every accepted connection. 0 disables.
    // Defeats slowloris-style attacks that hold connections open with little or
    // no traffic. Set before start() (or before connection arrive).
    void setIdleTimeout(uint32_t idle_seconds)
    {
        idle_seconds_ = idle_seconds;
    }

    // Apply a write timeout (seconds) to every connection: force-close a slow
    // consumer whose output cannot drain in time. 0 disables
    void setWriteTimeout(uint32_t write_seconds) { write_seconds_ = write_seconds; }

    // Cap each connection's input buffer (bytes); force-close on exceed to
    // bound read-side memory. 0 = unlimited.
    void setMaxInputBufferSize(size_t max_bytes)
    {
        max_input_buffer_size_ = max_bytes;
    }

    // Cap each connection's lifetime (seconds); recycle after this age.
    // 0 disables.
    void setMaxConnectionAge(uint32_t max_age_seconds)
    {
        max_age_seconds_ = max_age_seconds;
    }

    // Cap the number of concurrent connections. New connections beyond the cap
    // are accepted-then-immediately-closed so the accept queue does not wedge,
    // while bounding fd/memory consumption. 0 (default) means unlimited.
    void setMaxConnections(size_t max_connections)
    {
        max_connections_ = max_connections;
    }

    // Overload notification. Invoked (on the base loop) when the server sheds a
    // Connection due to overload -- either the connection cap or, if set, an
    // accept-rate limit. The application can use this for metrics/alerts. Cox
    // has already closed the shed connection's fd; the app connot respond on it
    // (there is no protocol context at the TCP layer). For request-level
    // shedding with a 503-style response, use ConcurrencyLimiter in the message
    // callback where request boundaries are known.
    using OverloadCallback = std::function<void(const InetAddress& peer)>;
    void setOverloadCallback(OverloadCallback cb)
    {
        overload_callback_ = std::move(cb);
    }

    // Optional accept-rate limit (new connections per second, with burst). When
    // exceeded, incoming connections are shed early (accept-then-close) and the
    // overload callback fires. 0 qps (default) disables rate limiting.
    void setAcceptRateLimit(double qps, double burst);
    size_t connectionCount() const { return connections_.size(); }

private:
    // Acceptor callback: a new connection fd is ready (runs on base loop).
    void newConnection(int sockfd, const InetAddress& peer);
    // TcpConnection close callback (may run on an io loop).
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    // Graceful-shutdown internals (run on the base loop).
    void shutdownInLoop(uint32_t drain_deadline_seconds);
    void forceCloseAllInLoop();
    void maybeFinishDrain();

    using ConnectionMap = std::map<std::string, TcpConnectionPtr>;

    reactor::EventLoop* loop_;  // base/acceptor loop
    const std::string ip_port_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::shared_ptr<reactor::EventLoopThreadPool> thread_pool_;

    ThreadInitCallback thread_init_callback_;
    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    WriteCompleteCallback write_complete_callback_;
    ConnectionCallback drain_callback_;
    DrainCompleteCallback drain_complete_callback_;
    OverloadCallback overload_callback_;

    // Accept-rate limiter (nullptr = disabled). Accessed only on the base loop.
    std::unique_ptr<flowcontrol::TokenBucket> accept_limiter_;

    std::atomic<bool> started_ {false};
    std::atomic<bool> draining_ {false};
    int next_conn_id_ {1};
    uint32_t idle_seconds_ {0};
    uint32_t write_seconds_ {0};
    uint32_t max_input_buffer_size_{0};
    uint32_t max_age_seconds_{0};
    uint32_t max_connections_{0};   // 0 - unlimited
    ConnectionMap connections_;     // accessed only on base loop
};
} // namespace net

