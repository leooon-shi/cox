#pragma once

#include "buffer.h"
#include "inet_address.h"
#include "io_channel.h"
#include "socket.h"
#include "timer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace reactor {
class EventLoop;
}

namespace net {
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// Callbacks exposed to the application.
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback =
    std::function<void(const TcpConnectionPtr&, buffer::Buffer* buf)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
// Invoked when the output buffer grows past a configured high-water mark, so
// the application can apply backpressure (stop producing). Reports the current
// number of bytes queued.
using HighWaterMarkCallback =
    std::function<void(const TcpConnectionPtr&, size_t bytes_queued)>;
// Internal: notifies the owner (TcpServer) that the connection is closed.
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;

// TcpConnection represents an established TCP connection. It is always managed
// by std::shared_ptr and uses shared_from_this() to keep itself alive across
// asynchronous callbacks (via Channel::tie). One TcpConnection belongs to
// exactly one EventLoop.
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(reactor::EventLoop* loop, std::string name, int sockfd,
                  const InetAddress& local, const InetAddress& peer);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    reactor::EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }

    // Generic per-connection context slot for higher-level protocols (e.g. an
    // HTTP parse context). Owned by the connection; set/read on the loop
    // thread.
    void setContext(std::shared_ptr<void> ctx) { context_ = std::move(ctx); }
    const std::shared_ptr<void>& getContext() const { return context_; }

    const InetAddress& localAddress() const { return local_addr_; }
    const InetAddress& peerAddress() const { return peer_addr_; }
    bool connected() const { return state_ == State::kConnected; }

    // Send data. Thread-safe: may be called from any thread.
    void send(const std::string& message);
    void send(const char* data, size_t len);

    // Half-close: shut down the write direction once pending output drains.
    void shutdown();

    // Force-close the connection (e.g. on idle timeout).
    void forceClose();

    void setTcpNoDelay(bool on) { socket_.setTcpNoDelay(on); }

    // Ingress backpressure: stop/resume reading from this connection.
    //
    // stopRead() disables the connection's read interest. The kernel receive
    // buffer then fills, the advertised TCP window shrinks to zero, and the
    // PEER's writes block -- throttling the sender without dropping data. Use
    // this when the application cannot keep up with an incoming stream.
    // startRead() resumes reading. Both are thread-safe (marshaled to the
    // loop).
    void stopRead();
    void startRead();
    bool isReading() const { return reading_; }

    // Enable an idle timeout: if no data is received for idle_seconds, the
    // connection is force-closed. This defeats slowloris-style attacks where a
    // peer holds a connection open while sending little or nothing. Reset on
    // every read. Pass 0 to disable. Must be called after connectEstablished
    // (typically from the connection callback) or it will be armed on connect.
    void setIdleTimeout(uint32_t idle_seconds) { idle_seconds_ = idle_seconds; }

    // Write timeout: if buffered output cannot be fully flushed within
    // write_seconds (i.e. the output buffer stays non-empty because the peer is
    // not reading), force-close the connection. Complements the high-water-mark
    // by bounding how long a slow consumer can hold resources. 0 disables.
    // The timer is armed when output first backs up and cancelled when it
    // drains.
    void setWriteTimeout(uint32_t write_seconds) {
        write_seconds_ = write_seconds;
    }

    // Maximum input buffer size (bytes). If the input buffer exceeds this after
    // a read -- e.g. a peer floods data faster than the app consumes it, or an
    // unframed/oversized message -- the connection is force-closed. Bounds
    // read-side memory. 0 (default) means unlimited.
    void setMaxInputBufferSize(size_t max_bytes) {
        max_input_buffer_size_ = max_bytes;
    }

    // Maximum connection lifetime (seconds). The connection is force-closed
    // after this age regardless of activity, to recycle long-lived connections
    // (e.g. rebalance after scaling, bound resource leaks). 0 disables. Armed
    // once at connectEstablished.
    void setMaxConnectionAge(uint32_t max_age_seconds) {
        max_age_seconds_ = max_age_seconds;
    }

    void setConnectionCallback(ConnectionCallback cb) {
        connection_callback_ = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb) {
        message_callback_ = std::move(cb);
    }
    void setWriteCompleteCallback(WriteCompleteCallback cb) {
        write_complete_callback_ = std::move(cb);
    }

    // Set the high-water-mark callback and the threshold (in bytes of queued
    // output) at which it fires. The callback fires on the upward crossing.
    void setHighWaterMarkCallback(HighWaterMarkCallback cb,
                                  size_t high_water_mark) {
        high_water_mark_callback_ = std::move(cb);
        high_water_mark_ = high_water_mark;
    }
    void setCloseCallback(CloseCallback cb) { close_callback_ = std::move(cb); }

    // Bytes currently queued in the output buffer (for backpressure decisions).
    size_t outputBufferPending() const;

    // Called by TcpServer once the connection object is fully constructed and
    // its shared_ptr exists; establishes the channel and fires the connection
    // callback. Must run in the loop thread.
    void connectEstablished();
    // Called by TcpServer when removing the connection; tears down the channel.
    void connectDestroyed();

private:
    enum class State { kConnecting, kConnected, kDisconnecting, kDisconnected };

    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    void resetIdleTimer();  //(re)arm the idle timeout on activity
    void cancelIdleTimer();

    void armWriteTimer();     // arm when output backs up
    void cancelWriteTimer();  // cancel when output drains
    void armMaxAgeTimer();    // arm once at establishment

    void sendInLoop(const char* data, size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();
    void startReadInLoop();
    void stopReadInLoop();

    void setState(State s) { state_ = s; }
    const char* stateToString() const;

    reactor::EventLoop* loop_;
    const std::string name_;
    std::atomic<State> state_;

    Socket socket_;
    reactor::Channel channel_;

    const InetAddress local_addr_;
    const InetAddress peer_addr_;

    buffer::Buffer input_buffer_;
    buffer::Buffer output_buffer_;

    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    WriteCompleteCallback write_complete_callback_;
    HighWaterMarkCallback high_water_mark_callback_;
    size_t high_water_mark_{64 * 1024 * 1024};  // 64 MB default
    CloseCallback close_callback_;

    // Idle timeout state (accessed only in the loop thread).
    uint32_t idle_seconds_{0};
    reactor::TimerId idle_timer_;
    bool idle_timer_armed_{false};

    // Write-timeout state (slow-consumer bound). Armed when output backs up.
    uint32_t write_seconds_{0};
    reactor::TimerId write_timer_;
    bool write_timer_armed_{false};

    // Max input buffer size (read-side memory bound). 0 = unlimited.
    size_t max_input_buffer_size_{0};

    // Max connection age (lifetime cap). Armed once at establishment.
    uint32_t max_age_seconds_{0};
    reactor::TimerId max_age_timer_;
    bool max_age_timer_armed_{false};

    // Ingress backpressure state (accessed only in the loop thread). Tracks
    // whether the connection's read interest is currently enabled.
    bool reading_{true};

    // Generic per-connection context for higher-level protocols.
    std::shared_ptr<void> context_;
};
}  // namespace net
