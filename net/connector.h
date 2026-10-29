#pragma once

#include "backoff.h"
#include "inet_address.h"
#include "timer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace reactor {
class EventLoop;
class Channel;
}  // namespace reactor

namespace net {
// Connector is the active-open side of the Acceptor-Connector pattern. It
// performs a non-blocking connect to a remote address, handling the
// asynchronous completion (writable => check SO_ERROR), self-connect
// detection, and retry with exponential backoff on failure.
//
// Dual-stack: a Connector may be given an ordered list of candidate addresses
// (e.g. a host's AAAA and A records). It attempts them in order; a failed
// attempt fails over to the next candidate IMMEDIATELY (no backoff), so a name
// whose first address is unreachable still connects via a later one. Only when
// the whole list is exhausted does it apply the jittered exponential backoff
// and retry from the top -- that backoff is for a genuinely-down endpoint, not
// for stepping between candidates. A bounded per-attempt timeout guards against
// a silently blackholed address stalling the walk.
//
// On success it hands the connected socket fd to a callback; the caller
// (TcpClient) then wraps it in a TcpConnection. Connector owns no connection
// state beyond the in-progress connect.
class Connector : public std::enable_shared_from_this<Connector> {
public:
    using NewConnectionCallback = std::function<void(int sockfd)>;

    Connector(reactor::EventLoop* loop, const InetAddress& server_addr);
    // Dual-stack: ordered candidates tried in turn on failure. Must be
    // non-empty.
    Connector(reactor::EventLoop* loop, std::vector<InetAddress> candidates);
    ~Connector();

    Connector(const Connector&) = delete;
    Connector& operator=(const Connector&) = delete;

    void setNewConnectionCallback(NewConnectionCallback cb) {
        new_connection_callback_ = std::move(cb);
    }

    // Per-attempt connect timeout in milliseconds. If a connect neither
    // completes nor errors within this window the attempt is abandoned and the
    // next candidate is tried. Default kConnectTimeoutMs.
    void setConnectTimeoutMs(uint64_t ms) { connect_timeout_ms_ = ms; }

    // Begin connecting. Safe to call from any thread.
    void start();

    // Retry after a failed attempt (used internally / for reconnection).
    void restart();

    // Stop connecting; cancels any pending retry.
    void stop();

    // The candidate currently being attempted.
    const InetAddress& serverAddress() const { return candidates_[current_]; }

private:
    enum class State { kDisconnected, kConnecting, kConnected };

    static constexpr int kInitRetryDelayMs = 500;
    static constexpr int kMaxRetryDelayMs = 30 * 1000;
    static constexpr uint64_t kConnectTimeoutMs = 1000;

    void setState(State s) { state_ = s; }
    void startInLoop();
    void stopInLoop();
    void connect();
    void connecting(int sockfd);
    void handleWrite();
    void handleError();
    // Called on a failed attempt: fail over to the next candidate immediately,
    // or (if the list is exhausted) back off and retry the whole list.
    void retry(int sockfd);
    void armConnectTimeout();
    void cancelConnectTimeout();
    void onConnectTimeout();
    int removeAndResetChannel();
    void resetChannel();
    // Stable backoff key for the whole candidate set (the endpoint identity),
    // so cycling through candidates counts as one failure, not one per address.
    const std::string& backoffKey() const { return backoff_key_; }

    reactor::EventLoop* loop_;
    const std::vector<InetAddress> candidates_;  // >= 1 entry
    size_t current_{0};                          // index being attempted
    const std::string backoff_key_;
    std::atomic<bool> connect_{false};
    std::atomic<State> state_;
    std::unique_ptr<reactor::Channel> channel_;
    NewConnectionCallback new_connection_callback_;
    // Jittered exponential backoff for reconnection. Keyed by the endpoint
    // identity so the same manager could be shared across connectors later;
    // jitter breaks the thundering herd when many clients reconnect after a
    // server restart.
    flowcontrol::Backoff backoff_;
    reactor::TimerId retry_timer_;
    bool retry_timer_armed_{false};
    uint64_t connect_timeout_ms_{kConnectTimeoutMs};
    reactor::TimerId connect_timer_;
    bool connect_timer_armed_{false};
};

using ConnectorPtr = std::shared_ptr<Connector>;
}  // namespace net
