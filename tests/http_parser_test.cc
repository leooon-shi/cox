#include "http_context.h"
#include "http_request.h"
#include "http_response.h"

#include "buffer.h"

#include <gtest/gtest.h>

#include <string>

using namespace http;

namespace
{
buffer::Buffer bufOf(const std::string& s)
{
    buffer::Buffer b;
    b.append(s);
    return b;
}
} // namespace

TEST(HttpParserTest, SimpleGet)
{
    HttpContext ctx;
    auto buf = bufOf("GET /hello?x=1 HTTP/1.1\r\nHost: localhost\r\n\r\n");
    ASSERT_TRUE(ctx.parseRequest(&buf));
    ASSERT_TRUE(ctx.gotAll());
    EXPECT_EQ(ctx.request().method(), Method::kGet);
    EXPECT_EQ(ctx.request().path(), "/hello");
    EXPECT_EQ(ctx.request().query(), "x=1");
    EXPECT_EQ(ctx.request().version(), "HTTP/1.1");
    EXPECT_EQ(ctx.request().header("host"), "localhost");
    EXPECT_TRUE(ctx.request().keepAlive());
}

TEST(HttpParserTest, PostWithContentLength)
{
    HttpContext ctx;
    auto buf = bufOf(
        "POST /submit HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world");
    ASSERT_TRUE(ctx.parseRequest(&buf));
    ASSERT_TRUE(ctx.gotAll());
    EXPECT_EQ(ctx.request().method(), Method::kPost);
    EXPECT_EQ(ctx.request().body(), "hello world");
}

TEST(HttpParserTest, PartialThenComplete)
{
    HttpContext ctx;
    // Feed the request in two pieces to exercise incremental parsing
    auto buf = bufOf("GET / HTTP/1.1\r\nHo");
    ASSERT_TRUE(ctx.parseRequest(&buf));
    EXPECT_FALSE(ctx.gotAll());     // header line incomplete
    buf.append("st: x\r\n\r\n");
    ASSERT_TRUE(ctx.parseRequest(&buf));
    EXPECT_TRUE(ctx.gotAll());
    EXPECT_EQ(ctx.request().header("host"), "x");
} 

TEST(HttpParserTest, ChunkedBody)
{
    HttpContext ctx;
    // "Wiki" + "pedia" in two chunks.
    auto buf = bufOf(
        "POST /c HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
    ASSERT_TRUE(ctx.parseRequest(&buf));
    ASSERT_TRUE(ctx.gotAll());
    EXPECT_EQ(ctx.request().body(), "Wikipedia");
}

TEST(HttpParserTest, MalformedMethodErrors)
{
    HttpContext ctx;
    auto buf = bufOf("FROBNICATE / HTTP/1.1\r\n\r\n");
    EXPECT_FALSE(ctx.parserRequest(&buf));
    EXPECT_TRUE(ctx.hasError());
}

TEST(HttpParserTest, BadVersionErrors)
{
    HttpContext ctx;
    auto buf = bufOf("GET / HTTP/9.9\r\n\r\n");
    EXPECT_FALSE(ctx.parseRequest(&buf));
    EXPECT_TRUE(ctx.hasError());
}

TEST(HttpParserTest, KeepAliveHttp100ptIn)
{
    HttpContext ctx;
    auto buf = bufOf("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    ASSERT_TRUE(ctx.parseRequest(&buf));
    ASSERT_TRUE(ctx.gotAll());
    EXPECT_TRUE(ctx.request().keepAlive());

    HttpContext ctx2;
    auto buf2 = bufOf("GET / HTTP/1.0\r\n\r\n");
    ASSERT_TRUE(ctx.parseRequest(&buf2));
    EXPECT_FALSE(ctx2.request().keepAlive());           // 1.0 defaults to close
}

TEST(HttpResponseTest, SerializesStatusHeaderBody)
{
    HttpResponse resp(/*keep_alive*/true);
    resp.setStatus(200);
    resp.setContentType("text/plain");
    resp.setBody("hi");

    buffer::Buffer out;
    resp.appendTo(&out);
    std::string s = out.retrieveAllAsString();

    EXPECT_NE(s.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(s.find("Content-Length: 2\r\n"), std::string::npos);
    EXPECT_NE(s.find("Connection: keep-alive\r\n"), std::string::npos);
    EXPECT_NE(s.find("Content-Type: text/plain\r\n"), std::string::npos);
    EXPECT_NE(s.find("\r\n\r\nhi"), std::string::npos);
}

TEST(HttpResponseTest, NotFoundReason)
{
    HttpResponse resp;
    resp.setStatus(404);
    buffer::Buffer out;
    resp.appendTo(&out);
    EXPECT_NE(out.retrieveAllAsString().find("404 Not Found"), std::string::npos);
}