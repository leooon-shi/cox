#include "router.h"

#include "http_request.h"
#include "http_response.h"

#include <gtest/gtest.h>

#include <string>

using namespace http;

namespace {
HttpRequest makeReq(Method m, const std::string& path)
{
    HttpRequest r;
    r.setMethod(m);
    r.setPath(path);
    return r;
}
}   // namespace

TEST(RouterTest, StaticRouteMatch)
{
    Router router;
    std::string hit;
    router.get("/health", [&](const HttpRequest&, const PathParams&,
                              HttpResponse* resp) {
        hit = "health";
        resp->setStatus(200);
    });

    HttpResponse resp;
    EXPECT_TRUE(router.route(makeReq(Method::kGet, "/health"), &resp));
    EXPECT_EQ(hit, "health");
    EXPECT_EQ(resp.statusCode(), 200);
}

TEST(RouterTest, PathParameterCapture)
{
    Router router;
    std::string captured_id;
    router.get("/users/{id}", [&](const HttpRequest&, const PathParams& p, 
                                  HttpResponse* resp) {
        captured_id = p.at("id");
        resp->setStatus(200);
    });

    HttpResponse resp;
    EXPECT_TRUE(router.route(makeReq(Method::kGet, "/users/42"), &resp));
    EXPECT_EQ(captured_id, "42");
}

TEST(RouterTest, MultipleParams)
{
    Router router;
    PathParams got;
    router.get("/orgs/{org}/repos/{repo}",
               [&](const HttpRequest&, const PathParams& p, HttpResponse* resp) {
        got = p;
        resp->setStatus(200);
    });

    HttpResponse resp;
    EXPECT_TRUE(router.route(makeReq(Method::kGet, "/orgs/acme/repos/cox"), &resp));
    EXPECT_EQ(got.at("org"), "acme");
    EXPECT_EQ(got.at("repo"), "cox");
}

TEST(RouterTest, NotFound)
{
    Router router;
    router.get("/a", [](const HttpRequest&, const PathParams&,
                        HttpResponse* resp) { resp->setStatus(200); });
    HttpResponse resp;
    EXPECT_FALSE(router.route(makeReq(Method::kGet, "/b"), &resp));
    EXPECT_EQ(resp.statusCode(), 404);
}

TEST(RouterTest, MethodNotAllowed)
{
    Router router;
    router.get("/res", [](const HttpRequest&, const PathParams&, HttpResponse* resp) { resp->setStatus(200); });

    HttpResponse resp;
    // Path exists but not for POST -> 405, not 404.
    EXPECT_FALSE(router.route(makeReq(Method::kPost, "/res"), &resp));
    EXPECT_EQ(resp.statusCode(), 405);
}

TEST(RouterTest, MethodDispatch)
{
    Router router;
    std::string which;
    router.get("/x", [&](const HttpRequest&, const PathParams&,
                        HttpResponse* r) { which = "get"; r->setstatus(200); });
    router.get("/x", [&](const HttpRequest&, const PathParams&,
                        HttpResponse* r) { which = "post"; r->setStatus(201); });
    
    HttpResponse resp;
    router.route(makeReq(Method::kPost, "/x"), &resp);
    EXPECT_EQ(which, "post");
    EXPECT_EQ(resp.statusCode(), 201);
}