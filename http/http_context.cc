#include "http_context.h"

#include "buffer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace http {
namespace {
// Find CRLF in [begin, end); returns pointer to the '\r' or nullptr.
const char* findCrlf(const char* begin, const char* end) {
    const char* p = begin;
    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n') return p;
        ++p;
    }
    return nullptr;
}
}  // namespace

bool HttpContext::parseRequestLine(const char* begin, const char* end) {
    // METHOD SP path[?query] SP HTTP/x.y
    const char* space1 = std::find(begin, end, ' ');
    if (space1 == end) return false;
    Method m = methodFromString(std::string(begin, space1));
    if (m == Method::kInvalid) return false;
    request_.setMethod(m);

    const char* target = space1 + 1;
    const char* space2 = std::find(target, end, ' ');
    if (space2 == end) return false;

    // Split path and optional query.
    const char* q = std::find(target, space2, '?');
    request_.setPath(std::string(target, q));
    if (q != space2) {
        request_.setQuery(std::string(q + 1, space2));
    }

    std::string version(space2 + 1, end);
    if (version != "HTTP/1.0" && version != "HTTP/1.1") return false;
    request_.setVersion(version);
    return true;
}

bool HttpContext::parseRequest(buffer::Buffer* buf) {
    bool progress = true;
    while (progress) {
        progress = false;

        if (state_ == State::kExpectRequestLine) {
            const char* crlf =
                findCrlf(buf->peek(), buf->peek() + buf->readableBytes());

            if (crlf) {
                if (!parseRequestLine(buf->peek(), crlf)) {
                    state_ = State::kError;
                    return false;
                }
                header_bytes_ += (crlf + 2) - buf->peek();
                buf->retrieve((crlf + 2) - buf->peek());
                state_ = State::kExpectHeaders;
                progress = true;
            }
        } else if (state_ == State::kExpectHeaders) {
            const char* crlf =
                findCrlf(buf->peek(), buf->peek() + buf->readableBytes());
            if (crlf) {
                if (crlf == buf->peek()) {
                    // Blank line -> headers complete. Decide body framing.
                    buf->retrieve(2);
                    std::string te = request_.header("transfer-encoding");
                    std::transform(
                        te.begin(), te.end(), te.begin(),
                        [](unsigned char c) { return std::tolower(c); });
                    if (te.find("chunked") != std::string::npos) {
                        state_ = State::kExpectChunkSize;
                    } else {
                        std::string cl = request_.header("content-length");
                        content_length_ =
                            cl.empty() ? 0
                                       : static_cast<size_t>(std::strtoul(
                                             cl.c_str(), nullptr, 10));
                        body_received_ = 0;
                        state_ = (content_length_ > 0) ? State::kExpectBody
                                                       : State::kGotAll;
                    }
                    progress = true;
                } else {
                    const char* colon = std::find(buf->peek(), crlf, ':');
                    if (colon == crlf) {
                        state_ = State::kError;  // malformed header line
                        return false;
                    }
                    std::string key(buf->peek(), colon);
                    const char* vbegin = colon + 1;
                    while (vbegin < crlf && *vbegin == ' ')
                        ++vbegin;  // trim SP
                    request_.addHeader(std::move(key),
                                       std::string(vbegin, crlf));
                    header_bytes_ += (crlf + 2) - buf->peek();
                    if (header_bytes_ > kMaxHeaderBytes) {
                        state_ = State::kError;
                        return false;
                    }
                    buf->retrieve((crlf + 2) - buf->peek());
                    progress = true;
                }
            }
        } else if (state_ == State::kExpectBody) {
            size_t avail = buf->readableBytes();
            size_t need = content_length_ - body_received_;
            size_t take = std::min(avail, need);
            if (take > 0) {
                request_.appendBody(buf->peek(), take);
                buf->retrieve(take);
                body_received_ += take;
                progress = true;
            }
            if (body_received_ == content_length_) {
                state_ = State::kGotAll;
            }
        } else if (state_ == State::kExpectChunkSize) {
            const char* crlf =
                findCrlf(buf->peek(), buf->peek() + buf->readableBytes());
            if (crlf) {
                // Hex chunk size (ignore any ;ext).
                std::string line(buf->peek(), crlf);
                chunk_remaining_ = static_cast<size_t>(
                    std::strtoul(line.c_str(), nullptr, 16));
                buf->retrieve((crlf + 2) - buf->peek());
                state_ = (chunk_remaining_ == 0) ? State::kGotAll
                                                 : State::kExpectChunkData;
                progress = true;
            }
        } else if (state_ == State::kExpectChunkData) {
            size_t avail = buf->readableBytes();
            size_t take = std::min(avail, chunk_remaining_);
            if (take > 0) {
                request_.appendBody(buf->peek(), take);
                buf->retrieve(take);
                chunk_remaining_ -= take;
                progress = true;
            }
            if (chunk_remaining_ == 0) {
                state_ = State::kExpectChunkCrlf;
            }
        } else if (state_ == State::kExpectChunkCrlf) {
            if (buf->readableBytes() >= 2) {
                buf->retrieve(2);  // Trailing CRLF after chunk data
                state_ = State::kExpectChunkSize;
                progress = true;
            }
        }
        // kGotAll / kError: stop.
    }
    return state_ != State::kError;
}

void HttpContext::reset() {
    state_ = State::kExpectRequestLine;
    request_.reset();
    content_length_ = 0;
    body_received_ = 0;
    header_bytes_ = 0;
    chunk_remaining_ = 0;
}
}  // namespace http
