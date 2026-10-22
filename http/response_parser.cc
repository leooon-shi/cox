#include "response_parser.h"

#include "buffer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace http
{
namespace
{
const char* findCrlf(const char* begin, const char* end)
{
    const char* p = begin;
    while( p + 1 < end)
    {
        if (p[0] == '\r' && p[1] == '\n') return p;
        ++p;
    }
    return nullptr;
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

bool ResponseParser::parseStatusLine(const char* begin, const char* end)
{
    // HTTP/1.1 <code> <reason>
    const char* sp1 = std::find(begin, end, ' ');
    if (sp1 == end)
    {
        return false;
    }
    std::string version(begin, sp1);
    if (version != "HTTP/1.0" && version != "HTTP/1.1")
    {
        return false;
    }

    const char* code_begin = sp1 +1;
    const char* sp2 = std::find(code_begin, end, ' ');
    std::string code_str(code_begin, sp2 == end ? end : sp2);
    int code = std::atoi(code_str.c_str());
    if ( code < 100 || code > 599)
    {
        return false;
    }

    std::string reason = (sp2 == end) ? std::string() : std::string(sp2 + 1, end);
    response_.setStatus(code, reason);
    return true;
}

bool ResponseParser::parse(buffer::Buffer* buf)
{
    bool progress = true;
    while (progress)
    {
        progress = false;
        if (state_ == State::kExpectStatusLine)
        {
            const char* crlf = findCrlf(buf->peek(),
                                        buf->peek() + buf->readableBytes());
            if (crlf)
            {
                if (!parseStatusLine(buf->peek(), crlf))
                {
                    state_ = State::kError;
                    return false;
                }
                buf->retrieve((crlf + 2) - buf->peek());
                state_ = State::kExpectHeaders;
                progress = true;
            }
        } 
        else if (state_ == State::kExpectHeaders)
        {
            const char* crlf = findCrlf(buf->peek(),
                                        buf->peek() + buf->readableBytes());
            if (crlf)
            {
                if (crlf == buf->peek())
                {
                    buf->retrieve(2);   // end of headers
                    auto te = lower_headers_.find("transfer-encoding");
                    if (te != lower_headers_.end() && te->second.find("chunked") != std::string::npos)
                    {
                        state_ = State::kExpectChunkSize;
                    } 
                    else if (has_content_length_)
                    {
                        state_ = (content_length_ > 0) ? State::kExpectBody : State::kGotAll;
                    } else {
                        // No framing header: body ends at EOF (Connection::close).
                        state_ = State::kExpectEofBody;
                    }
                    progress = true;
                } else {
                    const char* colon = std::find(buf->peek(), crlf, ':');
                    if (colon == crlf) { state_ = State::kError; return false; }
                    std::string key(buf->peek(), colon);
                    const char* vb  = colon +1;
                    while (vb < crlf && *vb == ' ') ++vb;
                    std::string value(vb, crlf);
                    response_.addHeader(key, value);
                    std::string lkey = toLower(key);
                    lower_headers_[lkey] = value;
                    if (lkey == "content-length")
                    {
                        has_content_length_ = true;
                        content_length_ = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
                    }
                    header_bytes_ += (crlf + 2) - buf->peek();
                    if (header_bytes_ > kMaxHeaderBytes)
                    {
                        state_ = State::kError;
                        return false;
                    }
                    buf->retrieve((crlf + 2) - buf->peek());
                    progress = true;
                }
            }
        }
        else if (state_ == State::kExpectBody)
        {
            size_t take = std::min(buf->readableBytes(), content_length_ - body_received_);

            if (take > 0)
            {
                body_accum_.append(buf->peek(), take);
                buf->retrieve(take);
                body_received_ += take;
                progress = true;
            }

            if (body_received_ == content_length_)
            {
                response_.setBody(std::move(body_accum_));
                state_ = State::kGotAll;
            }
        }
        else if (state_ == State::kExpectEofBody)
        {
            size_t avail = buf->readableBytes();
            if (avail > 0)
            {
                body_accum_.append(buf->peek(), avail);
                buf->retrieve(avail);
                progress = true;
            }
            // Completion happens in onEof().
        }
        else if (state_ == State::kExpectChunkSize)
        {
            const char* crlf = findCrlf(buf->peek(), buf->peek() + buf->readableBytes());

            if (crlf)
            {
                std::string line(buf->peek(), crlf);
                chunk_remaining_ = static_cast<size_t>(std::strtoul(line.c_str(), nullptr, 16));
                buf->retrieve((crlf+2) - buf->peek());
                if (chunk_remaining_ == 0)
                {
                    response_.setBody(std::move(body_accum_));
                    state_ = State::kGotAll;
                }
                else
                {
                    state_ = State::kExpectChunkData;
                }
                progress = true;
            }
        }
        else if (state_ == State::kExpectChunkData)
        {
            size_t take = std::min(buf->readableBytes(), chunk_remaining_);
            if (take > 0)
            {
                body_accum_.append(buf->peek(), take);
                buf->retrieve(take);
                chunk_remaining_ -= take;
                progress = true;
            }
        }
        else if (state_ == State::kExpectChunkCrlf)
        {
            if (buf->readableBytes() >= 2)
            {
                buf->retrieve(2);
                state_ = State::kExpectChunkSize;
                progress = true;
            }
        }
    }
    return state_ != State::kError;
}

void ResponseParser::onEof()
{
    if (state_ == State::kExpectEofBody)
    {
        response_.setBody(std::move(body_accum_));
        state_ = State::kGotAll;
    }
}

void ResponseParser::reset()
{
    state_ = State::kExpectStatusLine;
    response_ = HttpResponse();
    lower_headers_.clear();
    content_length_ = 0;
    body_received_ = 0;
    header_bytes_ = 0;
    chunk_remaining_ = 0;
    has_content_length_ = false;
    body_accum_.clear();
}
} // namespace http