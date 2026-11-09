#pragma once

#include "http_request.h"
#include "http_response.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace reactor {
class EventLoop;
}

namespace http {

class ServerStream;
using ServerStreamPtr = std::shared_ptr<ServerStream>;

// One HTTP request/response exchange on the server, for handlers that answer
// asynchronously and/or stream the response body. Same API on HTTP/1.1 and
// HTTP/2. Design: docs/insights/features/http-streaming-design.md.
//
//  server.setStreamHandler([](const http::ServerStreamPtr& s) {
//      s->writeHead(200, {{"content-type", "text/event-stream"}});
//      s->write("data: hello\n\n");         // sent now
//      // ...keep `s`, write more later, then:
//      s->send();
//  });
//
// Contract:
//  * Call it only on loop() (use loop()->runInLoop from other threads).
//  * Exactly one response: respond() once, OR writeHead(), write()*, end().
//    Misuse is logged and ignored.
//  * Holding the ServerStreamPtr keeps the exchange open. If every
//    reference is dropped without a response, the server answers 500, so a
//    forgetten response never hangs the client.
//  * onAbort fires at most once, when the client goes away (connection
//    closes, HTTP/2 RST_STREAM) before end(). Writes after it are ignored.
//  * Callbacks may capture the ServerStreamPtr itself (e.g. to keep
//    writing from onWritable). The stream drops its callbacks when the
//    exchange ends (end(), respond(), or abort), which breaks that cycle.
//    A stream that never ends, held only by its own callback, still leaks:
//    always end() or let the client abort.
class ServerStream {
public:
    using Headers = std::map<std::string, std::string>;

    // What the transport provides to a stream. Implemented by the HTTP/2
    // session and the HTTP/1.1 connection; not for application use.
    class Sink {
    public:
        virtual ~Sink() = default;
        virtual void sendHead(int status, const Headers& headers, bool has_body,
                              bool chunked_unknown_length) = 0;
        // Returns bytes now buffered for this stream (for backpressure).
        virtual size_t sendData(std::string_view data) = 0;
        virtual void sendEnd() = 0;
        // Called when the stream object is destroyed without a response.
        virtual void abandoned() = 0;
        virtual reactor::EventLoop* loop() const = 0;
    };

    ServerStream(HttpRequest request, std::shared_ptr<Sink> sink,
                 size_t high_water);
    ~ServerStream();

    ServerStream(const ServerStream&) = delete;
    ServerStream& operator=(const ServerStream&) = delete;

    const HttpRequest& request() const { return request_; }

    // ---- response ----
    // Send a whole response at once (stats, headers, body).
    void respond(const HttpResponse& resp);
    // Start a streamed response. If `headers` has no content-length the
    //  body length is open-ended: HTTP/2 just sends DATA frames, HTTP/1.1
    //  uses chunked transfer encoding.
    void writeHead(int status, Headers headers = {});
    // Append body bytes (copied). Return false when the bytes buffered for
    // this stream are at or above the high-water mark: the data is still
    // accepted, but the caller should wait for onWritable before writing
    // more.
    bool write(std::string_view data);
    // Finish the response body.
    void end();
    // Fires when buffered bytes drop below the high-water mark after a
    // write() returned false.
    void setOnWritable(std::function<void()> cb) {
        on_writable_ = std::move(cb);
    }

    // ---- lifetime ----
    void onAbort(std::function<void()> cb);
    bool aborted() const { return aborted_; }
    bool finished() const { return state_ == State::kEnded; }
    reactor::EventLoop* loop() const;

    // ---- transport side (called by the Sink owner) ----
    void notifyAbort();
    void notifyWritable(size_t buffered);

private:
    enum class State { kNew, kStreaming, kEnded };

    HttpRequest request_;
    std::shared_ptr<Sink> sink_;
    size_t high_water_;
    State state_{State::kNew};
    bool aborted_{false};
    bool want_writable_{false};
    std::function<void()> on_writable_;
    std::function<void()> on_abort_;
};

}  // namespace http
