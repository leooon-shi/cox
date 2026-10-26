#include "http_response.h"

#include "buffer.h"

#include <string>

namespace http {
const char* HttpResponse::defaultReason(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 413:
            return "Payload Too Large";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 503:
            return "Service Unavailable";
        default:
            return "OK";
    }
}

void HttpResponse::appendTo(buffer::Buffer* buf) const {
    std::string head;
    head.reserve(128 + headers_.size() * 32);

    head += "HTTP/1.1 ";
    head += std::to_string(status_code_);
    head += " ";
    head += status_reason_;
    head += "\r\n";

    // Content-Length is authoritative (we always send a fully-buffered body).
    head += "Content-Length: ";
    head += std::to_string(body_.size());
    head += "\r\n";

    head += "Connection: ";
    head += keep_alive_ ? "keep-alive" : "close";
    head += "\r\n";

    for (const auto& [key, value] : headers_) {
        // Skip headers we manage ourselves to avoid duplicates.
        if (key == "Content-Length" || key == "Connection") continue;
        head += key;
        head += ": ";
        head += value;
        head += "\r\n";
    }

    head += "\r\n";  // end of headers

    buf->append(head);
    if (!body_.empty()) {
        buf->append(body_);
    }
}

}  // namespace http
