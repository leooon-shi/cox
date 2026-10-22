#pragma once

#include "http_request.h"
#include "http_response.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace http
{
// Path parameters extracted from a matched route (e.g. {id} -> "42").
using PathParams = std::map<std::string, std::string>;

// A REST router: registers handlers by method + path pattern and dispatches
// matching requests. patterns support named segments: "/users/{id}".
//
// Matching is exact per path segment, with {name} capturing one segment into
// PathParams. This covers typical REST routing without a full regex engine.
class Router
{
public:
    using Handler = std::function<void(const HttpRequest& req,
                                       const PathParams& params,
                                        HttpResponse* resp)>;
    void addRoute(Method method, const std::string& pattern, Handler handler);
    
    // Convenience helpers
    void get(const std::string& pattern, Handler h) { addRoute(Method::kGet, pattern, std::move(h)); }
    void post(const std::string& pattern, Handler h) { addRoute(Method::kPost, pattern, std::move(h)); }
    void put(const std::string& pattern, Handler h) { addRoute(Method::kPut, pattern, std::move(h)); }
    void del(const std::string& pattern, Handler h) { addRoute(Method::kDelete, pattern, std::move(h)); }

    // Dispatch a request. Fills resp; sets 404 if no path matches, 405 if the
    // path matches but not for this method. Returns true if a handler ran.
    bool route(const HttpRequest& req, HttpResponse* resp) const;

private:
    struct Route {
        Method method;
        std::vector<std::string> segments;  // pattern split by '/'
        std::vector<bool> is_param;         // segment is {name}
        Handler handler;
    };

    static std::vector<std::string> splitPath(const std::string& path);
    bool matchRoute(const Route& r, const std::vector<std::string>& path_segs,
                    PathParams* params) const;
    
    std::vector<Route> routes_;
};

} // namespace http