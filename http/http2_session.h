#pragma once

#include "http_request.h"
#include "http_response.h"
#include "server_stream.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

struct nghttp2_session;

namespace reactor {
class EventLoop;
}

namespace http {

// Http2Session is the server side of ONE HTTP/2 connection (RFC 9113), built
// on nghttp2 (framing, HPACK, flow control, stream state). Only compiled when
// COX_WITH_HTTP2 is ON. Design: docs/insights/features/http2-design.md.
//
// It is SOCKET-FREE, like TlsEngine: decrypted bytes go in through feed(),
// frames come out through the `send` callback, so it can be unit-tested by
// piping it to an nghttp2 client session. HttpServer wires it to a
// TlsConnection once ALPN has selected "h2".
//
// Each request stream is turned into the same HttpRequest/HttpResponse the
// HTTP/1.1 path uses and handed to the same handler, so one handler serves
// both protocols. Streams are multiplexed: several can be in flight on one
// connection and their responses are written in completion order.
//
// Containment (same discipline as the HTTP/1.1 parser and TCP hardening):
//   * SETTINGS advertise max concurrent streams and max header list size;
//     nghttp2 refuses streams over the cap (RST_STREAM REFUSED_STREAM).
//   * A header list over the cap gets 431 and never reaches the handler.
//   * A request body over the cap gets 413 and the rest of it is dropped.
//   * Output backpressure: frames are only pulled from nghttp2 while the
//     downstream `pending()` bytes are below `output_high_water`; call
//     flush() again when the output drains (write-complete).
//   * nghttp2 itself enforces HTTP/2 flow-control windows, validates
//     pseudo-headers, and rate-limits stream resets ("rapid reset").
//
// Threading: not thread-safe. Use it from the connection's loop thread only.
class Http2Session {
public:
    using Handler =
        std::function<void(const HttpRequest& req, HttpResponse* resp)>;

    struct Callbacks {
        // Application handler. Runs synchronously per request: the response
        // must be complete when it returns. Ignored if stream_handler is set.
        Handler handler;
        // Async / streaming handler: receives a ServerStream and answers now
        // or later (see server_stream.h). Takes precedence over `handler`.
        std::function<void(const ServerStreamPtr&)> stream_handler;
        // The connection's loop, reported by ServerStream::loop() so a
        // handler can marshal work back. Optional for synchronous use.
        reactor::EventLoop* loop = nullptr;
        // Write wire bytes to the peer (required).
        std::function<void(const char* data, size_t len)> send;
        // Bytes still queued downstream (optional; enables backpressure).
        std::function<size_t()> pending;
        // Called after each request handled or rejected, with the status
        // sent and handler wall time (0 for rejected requests). Optional.
        std::function<void(int status, double handler_seconds)> on_request;
        // Called when a request is rejected before the handler (400, 413,
        // 431). Optional.
        std::function<void(int status)> on_reject;
    };

    // All HTTP/2 tunables. Defaults follow nginx / Envoy practice. Values
    // marked SETTINGS are advertised to the peer in the initial SETTINGS
    // frame (RFC 9113 §6.5.2); the rest are enforced locally.
    struct Limits {
        // SETTINGS_MAX_CONCURRENT_STREAMS: open streams the peer may have at
        // once. Excess streams are refused with RST_STREAM(REFUSED_STREAM),
        // which a client may safely retry.
        uint32_t max_concurrent_streams = 100;
        // SETTINGS_MAX_HEADER_LIST_SIZE: decoded header list size (name +
        // value + 32 per field). Larger lists get 431.
        size_t max_header_list_bytes = 64 * 1024;
        // SETTINGS_INITIAL_WINDOW_SIZE: per-stream receive window, i.e. how
        // much request body the peer may send before we acknowledge it.
        // RFC default 65535; max 2^31-1.
        uint32_t initial_window_size = 65535;
        // Connection-level receive window (sent as a WINDOW_UPDATE on stream
        // 0 after SETTINGS). 0 keeps the RFC default of 65535.
        uint32_t connection_window_size = 0;
        // SETTINGS_MAX_FRAME_SIZE: largest frame payload we accept.
        // 16384..16777215.
        uint32_t max_frame_size = 16384;
        // Request body cap (413). 0 = unlimited.
        size_t max_body_bytes = 8 * 1024 * 1024;
        // Highest client stream ID served on this connection. Client stream
        // IDs are odd and only increase, so this bounds the stream-ID space a
        // connection uses (the protocol maximum is 2^31-1). When a request
        // reaches this ID the server starts a graceful GOAWAY; streams above
        // it are refused and the client continues on a new connection.
        // 0 = protocol maximum.
        uint32_t max_stream_id = 0;
        // Requests served per connection before a graceful GOAWAY (nginx
        // keepalive_requests). Bounds per-connection state and spreads load
        // across connections. 0 = unlimited.
        uint32_t max_requests = 0;
        // Output backpressure threshold for pending() bytes.
        size_t output_high_water = 1024 * 1024;
        // Streamed responses: bytes buffered per stream before
        // ServerStream::write() returns false (backpressure).
        size_t stream_high_water = 256 * 1024;
    };

    Http2Session(Callbacks callbacks, Limits limits);
    ~Http2Session();

    Http2Session(const Http2Session&) = delete;
    Http2Session& operator=(const Http2Session&) = delete;

    // Validate `limits` against RFC 9113 bounds. Returns "" when valid,
    // otherwise a description of the first bad field.
    static std::string validate(const Limits& limits);

    // Send the server SETTINGS. Call once, before feeding any bytes. Returns
    // false if nghttp2 could not be initialised or the limits are invalid
    // (lastError() says which).
    bool start();

    // Feed decrypted bytes from the peer and send whatever the session
    // produces. Returns false on a fatal protocol error (e.g. not an HTTP/2
    // client preface); the caller should close the connection. lastError()
    // describes it.
    bool feed(const char* data, size_t len);

    // Pull pending frames out of nghttp2 and send them, stopping while the
    // downstream is above the high-water mark. Re-entrant calls are ignored.
    void flush();

    // Graceful shutdown (RFC 9113 §6.8), the two-step GOAWAY used by nginx
    // and Envoy. Call it twice:
    //   1st call: GOAWAY(last_stream_id = 2^31-1, NO_ERROR), a "shutdown
    //      notice". The client stops opening streams, but requests already in
    //      flight on the wire are still accepted.
    //   2nd call (about one round trip later; HttpServer uses a timer):
    //      GOAWAY(last_stream_id = highest stream received). Later streams
    //      are refused, and the client knows exactly which requests to retry
    //      on a new connection (those above last_stream_id).
    // Streams at or below last_stream_id run to completion; then finished()
    // turns true. Calls after the final GOAWAY do nothing. `immediate` sends
    // the final GOAWAY straight away (no notice). A reached max_stream_id or
    // max_requests calls this itself (1st step); the owner still makes the
    // 2nd call.
    void shutdown(bool immediate = false);

    // True once shutdown()/a limit has started GOAWAY.
    bool goingAway() const { return goaway_state_ != GoAway::kNone; }
    // True once the final GOAWAY (step 2) has been submitted.
    bool finalGoAwaySent() const { return goaway_state_ == GoAway::kFinalSent; }

    // True once neither side wants more I/O (after GOAWAY and all remaining
    // streams have finished, or a fatal error), The caller should then close
    // the connection.
    bool finished() const;

    const std::string lastError() const { return last_error_; }

    // Streams currently tracked (for tests).
    size_t activeStreams() const { return streams_.size(); }
    // Requests dispatched to the handler or rejected so far.
    uint64_t requestsServed() const { return requests_served_; }
    // Highest client stream ID received so far (0 if none).
    int32_t lastStreamId() const { return last_stream_id_; }

private:
    struct Stream;
    friend struct Http2SessionCallbacks;  // nghttp2 C-ABI trampolines (.cc)
    class H2Sink;  // ServerStream <-> HTTP/2 stream adapter (.cc)

    // Streaming-response plumbing, called through H2Sink.
    void streamHead(int32_t id, int status,
                    const ServerStream::Headers& headers, bool has_body,
                    bool no_body_method);
    size_t streamData(int32_t id, std::string_view data);
    void streamEnd(int32_t id);
    void streamAbandoned(int32_t id, bool head_sent);
    void notifyWritableStreams();
    bool streamExists(int32_t id) const;

    enum class GoAway { kNone, kNoticeQueued, kFinalSent };

    Stream* findStream(int32_t stream_id);
    void reject(Stream* s, int status);
    void dispatch(Stream* s);
    void submitResponse(Stream* s, const HttpResponse& resp, bool no_body);
    void countRequest();
    uint32_t effectiveMaxStreamId() const;

    Callbacks cbs_;
    Limits limits_;
    nghttp2_session* session_{nullptr};
    std::map<int32_t, std::unique_ptr<Stream>> streams_;
    bool flushing_{false};
    bool receiving_{false};         // inside nghttp2_session_mem_recv
    bool writable_pending_{false};  // a stream drained during mem_send
    GoAway goaway_state_{GoAway::kNone};
    uint64_t requests_served_{0};
    int32_t last_stream_id_{0};
    std::string last_error_;
    // Shared with every H2Sink: false once this session is destroyed, so a
    // ServerStream kept by a handler cannot touch a dead session.
    std::shared_ptr<bool> alive_;
};

}  // namespace http
