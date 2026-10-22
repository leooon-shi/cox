#pragma once

#include <map>
#include <string>
#include <utility>

namespace buffer{
class Buffer;
}

namespace http
{
// An HTTP response the application builds and the server serializes to the
// wire. Sets Content-Length automatically from the body on serialize.
class HttpResponse {
public:
    explicit HttpResponse(bool keep_alive = true): keep_alive_(keep_alive) {
    }
    void setStatus(int code, std::string reason = std::string())
    {
        status_code_ = code;
        status_reason_ = reason.empty() ? defaultReason(code) : std::move(reason);
    }
    int statusCode() const { return status_code_; }
    const std::string& statusReason() const { return status_reason_; }

    void addHeader(std::string key, std::string value) {
        headers_[std::move(key)] = std::move(value);
    }
    // Case-sensitive lookup as stored; returns empty if absent.
    std::string header(const std::string& key) const {
        auto it = headers_.find(key);
        return it == headers_.end() ? std::string() : it->second;
    }
    const std::map<std::string, std::string>& headers() const { return headers_; }

    void setContentType(const std::string& type)
    {
        addHeader("Content-Type", type);
    }

    void setBody(std::string body) { body_ = std::move(body); }
    const std::string& body() const { return body_; }

    void setKeepAlive(bool on) { keep_alive_ = on; }
    bool keepAlive() const { return keep_alive_; }

    // Serialize the full response (status line + headers + body) into buf.
    void appendTo(buffer::Buffer* buf) const;

    static const char* defaultReason(int code);

private:
    int status_code_ {200};
    std::string status_reason_ {"OK"};
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool keep_alive_;
};
}