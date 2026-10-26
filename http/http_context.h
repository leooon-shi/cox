#pragma once

#include "http_request.h"

#include <cstddef>

namespace buffer
{
class Buffer;
}

namespace http
{
// HttpContext holds the incremental parse state for one connection's inbound
// byte stream. parseRequest() consumes bytes from the Buffer as they arrive
// (handling partial reads and pipelined requests); when a full request is
// available, gotAll() becomes true and request() is complete.
//
// Supported: request line, headers, Content-Length bodies, and chunked
// transfer-encoding. Header/body size limits are enforced by the caller
// (HttpServer via TcpConnection's max-input-buffer) plus an internal header
// cap here.
class HttpContext {
public:
    enum class State
    {
        kExpectRequestLine,
        kExpectHeaders,
        kExpectBody,            // Content-Length body
        kExpectChunkSize,       // chunked: size line
        kExpectChunkData,       // chunked: chunk bytes
        kExpectChunkCrlf,       // chunked: trailing CRLF after a chunk
        kGotAll,
        kError
    };

    // Feed available bytes from buf into the parser. Returns false on a
    // malformed request (state becomes kError); the caller should respond 400
    // and close. Returns true otherwise (may or may not have a full request).
    bool parseRequest(buffer::Buffer* buf);

    bool gotAll() const { return state_ == State::kGotAll; }
    bool hasError() const { return state_ == State::kError; }

    HttpRequest& request() { return request_; }
    const HttpRequest& request() const { return request_; }

    // Reset for the next request on a keep-alive connection.
    void reset();

    // Maximum header section size (bytes) before rejecting (Dos bound).
    static constexpr size_t kMaxHeaderBytes = 64 * 1024;

private:
    bool parseRequestLine(const char* begin, const char* end);

    State state_{State::kExpectRequestLine};
    HttpRequest request_;
    size_t content_length_{0};
    size_t body_received_{0};
    size_t header_bytes_{0};
    size_t chunk_remaining_{0};
};
} // namespace http
