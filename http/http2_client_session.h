#pragma once

#include "http_request.h"
#include "http_response.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>

struct nghttp2_session;

namespace http {

// Http2ClientSession is the client side of ONE HTTP/2 connection (RFC 9113),
// built on nghttp2. Only compiled when COX_WITH_HTTP2 is ON.
//
// Socket-free, like Http2Session: bytes from the server go in through feed(),
// frames come out through the `send` callback. Http2Client wires it to a TLS
// connection and adds reconnection; tests pipe it to an Http2Session.
//
// Requests are multiplexed: submit() opens a new stream immediately (or queues
// the request while the server's MAX_CONCURRENT_STREAMS is reached) and each
// completes independently.
//
// Connection lifecycle -- each request completes exactly once, with:
//   * a response, or
//   * an error with `retryable == true` when the request was NOT processed by
//     the server and is safe to send again on a new connection:
//       - its stream ID is above the last_stream_id of a GOAWAY (RFC 9113
//         §6.8: the server did not and will not process it),
//       - the server refused it with RST_STREAM(REFUSED_STREAM) (§8.7),
//       - it was still queued locally when the connection ended;
//   * an error with `retryable == false` otherwise (e.g. the connection died
//     with the request in flight: it may have been processed).
// After GOAWAY, or when client stream IDs run out (they only increase, up to
// 2^31-1, and cannot be reused), submit() fails retryable at once and
// canAcceptRequests() turns false: the owner must continue on a new
// connection.
//
// Threading: not thread-safe. Use it from one thread (the connection's loop).
class Http2ClientSession {
public:
    struct Result {
        HttpResponse response;  // valid when error is empty
        std::string error;      // empty on success
        bool retryable{false};  // see class comment
    };
    using ResponseCallback = std::function<void(Result result)>;

    struct Callbacks {
        // Write wire bytes to the server (required).
        std::function<void(const char* data, size_t len)> send;
        // Bytes still queued downstream (optional; enables backpressure).
        std::function<size_t()> pending;
        // Called once when the session first learns it can no longer open
        // streams (GOAWAY received or stream IDs exhausted). Optional.
        std::function<void()> on_draining;
    };

    struct Limits {
        // Requests waiting locally for a free stream slot (beyond the
        // server's MAX_CONCURRENT_STREAMS). Excess submits fail retryable.
        size_t max_queued = 1000;
        // Response body cap; a larger body fails the request. 0 = unlimited.
        size_t max_response_body = 64 * 1024 * 1024;
        // SETTINGS_INITIAL_WINDOW_SIZE we advertise (download window).
        uint32_t initial_window_size = 65535;
        // Highest stream ID this session will use (odd, client side). Lower
        // it in tests to exercise ID exhaustion. 0 = protocol maximum.
        uint32_t max_stream_id = 0;
        // Output backpressure threshold for pending() bytes.
        size_t output_high_water = 1024 * 1024;
    };

    Http2ClientSession(Callbacks callbacks, Limits limits);
    ~Http2ClientSession();  // fails anything outstanding (retryable if unsent)

    Http2ClientSession(const Http2ClientSession&) = delete;
    Http2ClientSession& operator=(const Http2ClientSession&) = delete;

    // Send the connection preface and SETTINGS. Call once, before anything
    // else. Returns false if nghttp2 could not be initialised.
    bool start();

    // Submit a request. `authority` is the :authority pseudo-header (host or
    // host:port); `scheme` is "https" or "http". The callback fires exactly
    // once (possibly synchronously, e.g. if the session is draining).
    void submit(const HttpRequest& req, const std::string& authority,
                const std::string& scheme, ResponseCallback cb);

    // Feed bytes from the server. Returns false on a fatal protocol error;
    // outstanding requests have then been failed and the caller should close.
    bool feed(const char* data, size_t len);

    // Pull pending frames and send them (stops above the high-water mark).
    void flush();

    // The transport closed: fail everything still outstanding (retryable if
    // it never reached the server, non-retryable if it was in flight).
    void connectionLost(const std::string& why);

    // Gracefully stop: send GOAWAY to the server, keep receiving responses
    // for streams already open; new submits fail retryable.
    void shutdown();

    // False after GOAWAY (either side), stream-ID exhaustion, or a fatal
    // error. Requests must then go to a new connection.
    bool canAcceptRequests() const;
    // True when nothing is outstanding and the session wants no more I/O.
    bool finished() const;

    size_t inFlight() const { return streams_.size(); }
    size_t queued() const { return queue_.size(); }
    // True if a new request would get a stream now (below the server's
    // MAX_CONCURRENT_STREAMS and accepting requests).
    bool hasFreeStream() const;
    // last_stream_id of the GOAWAY received, or -1 if none.
    int32_t goAwayLastStreamId() const { return goaway_last_stream_id_; }
    const std::string& lastError() const { return last_error_; }

private:
    struct Stream;
    struct Pending;
    friend struct Http2ClientCallbacks;  // nghttp2 C-ABI trampolines (.cc)

    void pumpQueue();
    bool openStream(std::unique_ptr<Pending> p);
    void complete(int32_t stream_id, Result result);
    void markDraining();
    void failAll(const std::string& why, bool queued_retryable);
    uint32_t maxStreamId() const;

    Callbacks cbs_;
    Limits limits_;
    nghttp2_session* session_{nullptr};
    std::map<int32_t, std::unique_ptr<Stream>> streams_;
    std::deque<std::unique_ptr<Pending>> queue_;
    bool flushing_{false};
    bool draining_{false};
    bool dead_{false};
    int32_t goaway_last_stream_id_{-1};
    std::string last_error_;
};

}  // namespace http
