#pragma once

#include "http_response.h"

#include <cstddef>
#include <map>
#include <string>

namespace buffer
{
class Buffer;
};

namespace http
{
// Incremental parser for HTTP/1.1 responses (client side). Mirrors the request
// parser but expects a status line ("HTTP/1.1 200 OK") instead of a request
// line. Handles Content-Length and chunked bodies. For responses without a
// framed length (Connection: close, no Content-Length), the body is terminated
// by EOF -- the caller signals that via onEof().
class ResponseParser {
public:
    enum class State {
        kExpectStatusLine,
        kExpectHeaders,
        kExpectBody,
        kExpectChunkSize,
        kExpectChunkData,
        kExpectChunkCrlf,
        kExpectEofBody,   // body runs until connection clsoe
        kGotAll,
        kError
    };

    bool parse(buffer::Buffer* buf);
    // Called when the peer closes: finalizes an EOF-delimited body.
    void onEof();

    bool gotAll() const { return state_ == State::kGotAll; }
    bool hasError() const { return state_ == State::kError; }

    HttpResponse& response() { return response_; }
    const HttpResponse& response() const { return response_; }

    void reset();

    static constexpr size_t kMaxHeaderBytes = 64 * 1024;

private:
    bool parseStatusLine(const char* begin, const char* end);

    State state_{ State::kExpectStatusLine };
    HttpResponse response_;
    std::map<std::string, std::string> lower_headers_;  // for framing lookups
    size_t content_length_{0};
    size_t body_received_{0};
    size_t header_bytes_{0};
    size_t chunk_remaining_{0};
    bool has_content_length_{false};
    std::string body_accum_;
};

}   // namespace http
