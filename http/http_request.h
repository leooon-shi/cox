#include "url.h"

#include <map>
#include <string>
#include <utility>

namespace buffer {
class Buffer;
}

namespace http {
// Common HTTP methods. kInvalid marks an unparsed/unsupported method.
enum class Method { kInvalid, kGet, kPost, kPut, kDelete, kHead, kOptions, kPatch };

const char* methodToString(Method m);
Method methodFromString(const std::string& s);

// A parsed HTTP request. Headers are stored case-insensitively (lowercased
// keys) per RFC 7230. Body is the raw payload (after Content-Length /
// dechunking handled by the parser).
class HttpRequest {
public:
    void setMethod(Method m) { method_ = m; }
    Method method() const { return method_; }

    void setPath(std::string p)  { path_ = std::move(p); }
    const std::string& path() const { return path_; }

    void setQuery(std::string q)
    {
        query_ = std::move(q);
        query_parsed_ = false;
        query_params_.clear();
    }
    const std::string& query() const { return query_; }

    // Parsed + percent-decoded query string ("?a=1&b=2"). Parsed lazily on
    // first access and cached; setQuery() invalidates the cache.
    const QueryParams& queryParams() const;
    //Single query parameter, or empty string if absent.
    std::string queryParam(const std::string& key) const;
    bool hasQueryParam(const std::string& key) const;

    // Parsed application/x-www-form-urlencoded body. Returns empty params when
    // the Content-Type is not form-urlencoded.
    QueryParams formParams() const;

    void setVersion(std::string v) { version_ = std::move(v); }
    const std::string& version() const { return version_; }

    // Header keys are lowercased for case-insensitive lookup.
    void addHeader(std::string key, std::string value);
    // Returns the header value or empty string if absent
    std::string header(const std::string& key) const;
    bool hasHeader(const std::string& key) const;
    const std::map<std::string, std::string> & headers() const { return headers_; }

    void setBody(std::string b) { body_ = std::move(b); }
    void appendBody(const char* data, size_t len) { body_.append(data, len); }
    const std::string& body() const { return body_; }

    // Keep-alive per HTTP/1.1 defaults + Connection header.
    bool keepAlive() const;

    // Serialize this request to the wire (client side). `host` sets the Host
    // header if not already present. Adds Content-Length from the body.
    void appendTo(buffer::Buffer* buf, const std::string& host = "") const;

    void reset();
private:
    Method method_{Method::kInvalid};
    std::string path_;
    std::string query_;
    std::string version_ { "HTTP/1.1"};
    std::map<std::string, std::string> headers_;
    std::string body_;

    // Lazily-parsed query parameters (mutable: parsing is a const-observable
    // cache, not a change in logical state).
    mutable QueryParams query_params_;
    mutable bool query_parsed_ {false};
};
} // namespace http