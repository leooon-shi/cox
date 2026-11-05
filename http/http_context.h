#pragma once

#include "http_request.h"

#include <cstddef>

namespace buffer {
class Buffer;
}

namespace http {
// HttpContext holds the incremental parse state for one connection's inbound
// byte stream. parseRequest() consumes bytes from the Buffer as they arrive
// (handling partial reads and pipelined requests); when a full request is
// available, gotAll() becomes true and request() is complete.
//
// Supported: request line, headers, Content-Length bodies, and chunked
// transfer-encoding. The header section is bounded by kMaxHeaderBytes and the
// body by setMaxBodyBytes() (a real per-request limit, independent of the TCP
// input-buffer cap).
class HttpContext {
public:
    enum class State {
        kExpectRequestLine,
        kExpectHeaders,
        kExpectBody,       // Content-Length body
        kExpectChunkSize,  // chunked: size line
        kExpectChunkData,  // chunked: chunk bytes
        kExpectChunkCrlf,  // chunked: trailing CRLF after a chunk
        kGotAll,
        kError
    };

    // Feed available bytes from buf into the parser. Returns false on a
    // malformed request (state becomes kError); the caller should respond 400
    // and close. Returns true otherwise (may or may not have a full request).
    bool parseRequest(buffer::Buffer* buf);

    bool gotAll() const { return state_ == State::kGotAll; }
    bool hasError() const { return state_ == State::kError; }

    // Why the last parse failed. kTooLarge means the body exceeded the
    // configured max (respond 413); kBadRequest covers all other malformed
    // input (respond 400). Only meaningful when hasError() is true.
    enum class Error { kNone, kBadRequest, kTooLarge };
    Error error() const { return error_; }

    HttpRequest& request() { return request_; }
    const HttpRequest& request() const { return request_; }

    // Reset for the next request on a keep-alive connection.
    void reset();

    // Maximum request body size in bytes. 0 disables the limit. Enforced for
    // both Content-Length (rejected up front from the declared length) and
    // chunked (rejected once the accumulated body exceeds the cap). This is a
    // real per-request limit, distinct from the TCP input-buffer cap, which
    // only bounds unconsumed bytes in flight.
    void setMaxBodyBytes(size_t bytes) { max_body_bytes_ = bytes; }
    size_t maxBodyBytes() const { return max_body_bytes_; }

    // Maximum header section size (bytes) before rejecting (Dos bound).
    static constexpr size_t kMaxHeaderBytes = 64 * 1024;

    // Default body cap applied by HttpServer unless overridden (8 MiB).
    static constexpr size_t kDefaultMaxBodyBytes = 8 * 1024 * 1024;

private:
    bool parseRequestLine(const char* begin, const char* end);

    State state_{State::kExpectRequestLine};
    Error error_{Error::kNone};
    HttpRequest request_;
    size_t content_length_{0};
    size_t body_received_{0};
    size_t header_bytes_{0};
    size_t chunk_remaining_{0};
    size_t max_body_bytes_{0};  // 0 = unlimited
};
}  // namespace http
