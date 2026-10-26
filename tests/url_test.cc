// URL decoding / encoding, query-string parsing, and the HttpRequest +
// Router integration that exposes them.

#include "url.h"

#include "http_request.h"
#include "http_response.h"
#include "router.h"

#include <gtest/gtest.h>

#include <string>

using namespace http;

TEST(UrlTest, DecodePercentEscapes) {
    EXPECT_EQ(urlDecode("hello%20world"), "hello world");
    EXPECT_EQ(urlDecode("a%2Fb"), "a/b");
    EXPECT_EQ(urlDecode("%7Euser"), "~user");
    EXPECT_EQ(urlDecode("caf%C3%A9"), "caf\xC3\xA9");  // UTF-8 passthrough
}

TEST(UrlTest, DecodePlusHandling) {
    EXPECT_EQ(urlDecode("a+b"), "a b");  // form default
    EXPECT_EQ(urlDecode("a+b", /*plus_as_space=*/false), "a+b");  // path mode
}

TEST(UrlTest, DecodeMalformedEscapeIsKeptLiterally) {
    EXPECT_EQ(urlDecode("100%"), "100%");
    EXPECT_EQ(urlDecode("%zz"), "%zz");
    EXPECT_EQ(urlDecode("%A"), "%A");  // truncated escape at end
}

TEST(UrlTest, EncodeUnreservedPassThrough) {
    EXPECT_EQ(urlEncode("abcXYZ019-_.~"), "abcXYZ019-_.~");
    EXPECT_EQ(urlEncode("a b"), "a%20b");
    EXPECT_EQ(urlEncode("a/b?c=d&e"), "a%2Fb%3Fc%3Dd%26e");
}

TEST(UrlTest, EncodeDecodeRoundTrip) {
    const std::string raw = "key with spaces/&=?%+#";
    EXPECT_EQ(urlDecode(urlEncode(raw), /*plus_as_space=*/false), raw);
}

TEST(UrlTest, ParseQueryBasic) {
    auto p = parseQuery("a=1&b=2&c=three");
    EXPECT_EQ(p.size(), 3u);
    EXPECT_EQ(p["a"], "1");
    EXPECT_EQ(p["b"], "2");
    EXPECT_EQ(p["c"], "three");
}

TEST(UrlTest, ParseQueryDecodesKeysAndValues) {
    auto p = parseQuery("full+name=Ada+Lovelace&path=%2Fusr%2Fbin");
    EXPECT_EQ(p["full name"], "Ada Lovelace");
    EXPECT_EQ(p["path"], "/usr/bin");
}

TEST(UrlTest, ParseQueryEdgeCases) {
    auto p = parseQuery("flag&empty=&x=1");
    EXPECT_TRUE(p.count("flag"));
    EXPECT_EQ(p["flag"], "");
    EXPECT_TRUE(p.count("empty"));
    EXPECT_EQ(p["empty"], "");
    EXPECT_EQ(p["x"], "1");

    EXPECT_TRUE(parseQuery("").empty());
    EXPECT_TRUE(parseQuery("&&").empty());

    // Value containing '=' keeps everything after the first one.
    auto q = parseQuery("token=a=b=c");
    EXPECT_EQ(q["token"], "a=b=c");

    // Later duplicate wins.
    auto d = parseQuery("k=1&k=2");
    EXPECT_EQ(d["k"], "2");
}

TEST(UrlTest, RequestQueryParams) {
    HttpRequest req;
    req.setQuery("page=2&per_page=50&q=hello+world");

    EXPECT_EQ(req.queryParam("page"), "2");
    EXPECT_EQ(req.queryParam("per_page"), "50");
    EXPECT_EQ(req.queryParam("q"), "hello world");
    EXPECT_TRUE(req.hasQueryParam("page"));
    EXPECT_FALSE(req.hasQueryParam("missing"));
    EXPECT_EQ(req.queryParam("missing"), "");
    EXPECT_EQ(req.queryParams().size(), 3u);
}

TEST(UrlTest, RequestQueryCacheInvalidatedOnSet) {
    HttpRequest req;
    req.setQuery("a=1");
    EXPECT_EQ(req.queryParam("a"), "1");  // Populates the cache
    req.setQuery("a=2&b=3");
    EXPECT_EQ(req.queryParam("a"), "2");
    EXPECT_EQ(req.queryParam("b"), "3");

    req.reset();
    EXPECT_TRUE(req.queryParams().empty());
}

TEST(UrlTest, RequestFormParams) {
    HttpRequest req;
    req.setMethod(Method::kPost);
    req.addHeader("Content-Type", "application/x-www-form-urlencoded");
    req.setBody("user=ada&msg=hi+there");

    auto form = req.formParams();
    EXPECT_EQ(form["user"], "ada");
    EXPECT_EQ(form["msg"], "hi there");
}

TEST(UrlTest, RequestFormParamsIgnoresOtherContentTypes) {
    HttpRequest req;
    req.setMethod(Method::kPost);
    req.addHeader("Content-Type", "application/json");
    req.setBody("user=ada");
    EXPECT_TRUE(req.formParams().empty());
}

TEST(UrlTest, RouterDecodesPathParams) {
    Router router;
    std::string captured;
    router.get("/files/{name}", [&](const HttpRequest&, const PathParams& p,
                                    HttpResponse* resp) {
        captured = p.at("name");
        resp->setStatus(200);
    });

    HttpRequest req;
    req.setMethod(Method::kGet);
    req.setPath("/files/my%20report.txt");
    HttpResponse resp;
    EXPECT_TRUE(router.route(req, &resp));
    EXPECT_EQ(captured, "my report.txt");
}

TEST(UrlTest, RouterEncodedSlashStaysInOneSegment) {
    Router router;
    std::string captured;
    router.get("/files/{name}", [&](const HttpRequest&, const PathParams& p,
                                    HttpResponse* resp) {
        captured = p.at("name");
        resp->setStatus(200);
    });

    HttpRequest req;
    req.setMethod(Method::kGet);
    req.setPath("/files/a%2Fb");  // %2F must not split into two segments
    HttpResponse resp;
    EXPECT_TRUE(router.route(req, &resp));
    EXPECT_EQ(captured, "a/b");
}

TEST(UrlTest, RouterDoesNotTreatPlusAsSpaceInPath) {
    Router router;
    std::string captured;
    router.get("/tags/{tag}", [&](const HttpRequest&, const PathParams& p,
                                  HttpResponse* resp) {
        captured = p.at("tag");
        resp->setStatus(200);
    });

    HttpRequest req;
    req.setMethod(Method::kGet);
    req.setPath("/tags/c++");

    HttpResponse resp;
    EXPECT_TRUE(router.route(req, &resp));
    EXPECT_EQ(captured, "c++");
}
