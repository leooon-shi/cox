#include "http_request.h"

#include "buffer.h"

#include <algorithm>
#include <cctype>

namespace http
{
const char* methodToString(Method m)
{
    switch(m)
    {
        case Method::kGet: return "GET";
        case Method::kPost: return "POST";
        case Method::kPut: return "PUT";
        case Method::kDelete: return "DELETE";
        case Method::kHead: return "HEAD";
        case Method::kOptions: return "OPTIONS";
        case Method::kPatch: return "PATCH";
        case Method::kInvalid: default: return "INVALID";
    }
}

Method methodFromString(const std::string& s)
{
    if (s == "GET") return Method::kGet;
    if (s == "POST") return Method::kPost;
    if (s == "PUT") return Method::kPut;
    if (s == "DELETE") return Method::kDelete;
    if (s == "HEAD") return Method::kHead;
    if (s == "OPTIONS") return Method::kOptions;
    if (s == "PATCH") return Method::kPatch;
    return Method::kInvalid;
}

namespace
{
std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

void HttpRequest::addHeader(std::string key, std::string value)
{
    headers_[toLower(std::move(key))] = std::move(value);
}
std::string HttpRequest::header(const std::string& key) const {
    auto it = headers_.find(toLower(key));
    return it == headers_.end() ? std::string() : it->second;
}

bool HttpRequest::hasHeader(const std::string& key) const {
    return headers_.find(toLower(key)) != headers_.end();
}

const QueryParams& HttpRequest::queryParams() const {
    if (!query_parsed_)
    {
        query_params_ = parseQuery(query_);
        query_parsed_ = true;
    }
    return query_params_;
}

std::string HttpRequest::queryParam(const std::string& key) const
{
    const auto& p = queryParams();
    auto it = p.find(key);
    return it == p.end() ? std::string() : it->second;
}

bool HttpRequest::hasQueryParam(const std::string& key) const {
    return queryParams().count(key) != 0;
}

QueryParams HttpRequest::formParams() const
{
    std::string ct = toLower(header("content-type"));
    if (ct.find("application/x-www-form-urlencoded") == std::string::npos)
    {
        return QueryParams();
    }
    return parseQuery(body_);
}

bool HttpRequest::keepAlive() const
{
    std::string conn = header("connection");
    std::transform(conn.begin(), conn.end(), conn.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (version_ == "HTTP/1.1")
    {
        return conn != "close"; // keep-alive default in 1.1
    }
    return conn == "keep-alive";  // opt-in for 1.0
}

void HttpRequest::reset()
{
    method_ = Method::kInvalid;
    path_.clear();
    query_.clear();
    version_ = "HTTP/1.1";
    headers_.clear();
    body_.clear();
    query_params_.clear();
    query_parsed_ = false;
}

void HttpRequest::appendTo(buffer::Buffer* buf, const std::string& host) const
{
    std::string head;
    head.reserve(128 + body_.size());
    head += methodToString(method_);
    head += " ";
    head += path_.empty() ? "/" : path_;
    if (!query_.empty())
    {
        head += "?";
        head += query_;
    }
    head += " HTTP/1.1\r\n";

    if (!host.empty() && headers_.find("host") == headers_.end())
    {
        head += "Host: ";
        head += host;
        head += "\r\n";
    }
    for (const auto& [key, value] : headers_)
    {
        if (key == "content-length") continue;  // we set it below
        head += key;
        head += ": ";
        head += value;
        head += "\r\n";
    }
    head += "Content-Length: ";
    head += std::to_string(body_.size());
    head += "\r\n\r\n";

    buf->append(head);
    if (!body_.empty())
    {
        buf->append(body_);
    }
}
}   // namespace http
