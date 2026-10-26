#include "router.h"

#include "url.h"

#include <sstream>
#include <utility>

namespace http {
std::vector<std::string> Router::splitPath(const std::string& path) {
    std::vector<std::string> segs;
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/') {
            ++i;
            continue;
        }
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        segs.push_back(path.substr(i, j - i));
        i = j;
    }
    return segs;
}

void Router::addRoute(Method method, const std::string& pattern,
                      Handler handler) {
    Route r;
    r.method = method;
    r.segments = splitPath(pattern);
    r.is_param.reserve(r.segments.size());
    for (auto& seg : r.segments) {
        bool param = seg.size() >= 2 && seg.front() == '{' && seg.back() == '}';
        if (param) {
            seg = seg.substr(1, seg.size() - 2);  // strip braces -> param name
        }
        r.is_param.push_back(param);
    }
    r.handler = std::move(handler);
    routes_.push_back(std::move(r));
}

bool Router::matchRoute(const Route& r,
                        const std::vector<std::string>& path_segs,
                        PathParams* params) const {
    if (r.segments.size() != path_segs.size()) return false;
    PathParams captured;
    for (size_t i = 0; i < r.segments.size(); ++i) {
        if (r.is_param[i]) {
            // Decode the captured segment (e.g. "%20" -> " "). Decoding after
            // splitting means an encoded '/' (%2F) stays inside one segment
            // and cannot smuggle an extra path level.
            captured[r.segments[i]] = urlDecode(path_segs[i],
                                                /*plus_as_space=*/false);
        } else if (r.segments[i] != path_segs[i]) {
            return false;
        }
    }
    *params = std::move(captured);
    return true;
}

bool Router::route(const HttpRequest& req, HttpResponse* resp) const {
    auto path_segs = splitPath(req.path());
    bool path_matched = false;  // for 405 vs 404 distinction

    for (const auto& r : routes_) {
        PathParams params;
        if (matchRoute(r, path_segs, &params)) {
            path_matched = true;
            if (r.method == req.method()) {
                r.handler(req, params, resp);
                return true;
            }
        }
    }

    if (path_matched) {
        resp->setStatus(405);  // Method Not Allowed
        resp->setContentType("text/plain");
        resp->setBody("Method Not Allowed");
    } else {
        resp->setStatus(404);
        resp->setContentType("text/plain");
        resp->setBody("Not Found");
    }
    return false;
}

}  // namespace http
