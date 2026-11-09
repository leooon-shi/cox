// Socket-free tests for http::Http2ClientSession, piped byte-for-byte into
// Cox's own server session (http::HttpSession). Only built with
// COX_WITH_HTTP2.

#include "http2_client_session.h"
#include "http2_session.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using http::Http2ClientSession;
using http::Http2Session;
using http::HttpRequest;
using http::HttpResponse;

namespace {

HttpRequest makeGet(const std::string& path) {
    HttpRequest r;
    r.setMethod(http::Method::kGet);
    r.setPath(path);
    return r;
}

// A client session and a server joined by in-memory pipes.
struct Pair {
    std::string c2s;  // client -> server bytes
    std::string s2c;  // server -> client bytes
    int handled = 0;
    std::unique_ptr<Http2Session> server;
    std::unique_ptr<Http2ClientSession> client;
    std::vector<Http2ClientSession::Result> results;
    int draining_events = 0;

    explicit Pair(Http2Session::Limits slim = {},
                  Http2ClientSession::Limits clim = {},
                  Http2Session::Handler handler = nullptr) {
        Http2Session::Callbacks scb;
        scb.handler =
            handler
                ? handler
                : [this](const HttpRequest& req, HttpResponse* resp) {
                      ++handled;
                      resp->setBody("echo " + req.path() + " " + req.body());
                  };
        scb.send = [this](const char* d, size_t n) { s2c.append(d, n); };
        server = std::make_unique<Http2Session>(std::move(scb), slim);
        EXPECT_TRUE(server->start());

        Http2ClientSession::Callbacks ccb;
        ccb.send = [this](const char* d, size_t n) { c2s.append(d, n); };
        ccb.on_draining = [this] { ++draining_events; };
        client = std::make_unique<Http2ClientSession>(std::move(ccb), clim);
        EXPECT_TRUE(client->start());
    }

    Http2ClientSession::ResponseCallback collect() {
        return [this](Http2ClientSession::Result r) {
            results.push_back(std::move(r));
        };
    }

    void pump() {
        for (int i = 0; i < 100; ++i) {
            std::string up;
            up.swap(c2s);
            if (!up.empty()) {
                ASSERT_TRUE(server->feed(up.data(), up.size()))
                    << server->lastError();
            }
            std::string down;
            down.swap(s2c);
            if (!down.empty()) {
                ASSERT_TRUE(client->feed(down.data(), down.size()))
                    << client->lastError();
            }
            if (up.empty() && down.empty()) {
                return;
            }
        }
    }
};

}  // namespace

TEST(Http2ClientSessionTest, GetRoundTrip) {
    Pair p;
    p.client->submit(makeGet("/hello"), "localhost", "https", p.collect());
    p.pump();
    ASSERT_EQ(p.results.size(), 1u);
    EXPECT_EQ(p.results[0].error, "");
    EXPECT_EQ(p.results[0].response.statusCode(), 200);
    EXPECT_EQ(p.results[0].response.body(), "echo /hello ");
    EXPECT_EQ(p.client->inFlight(), 0u);
}

TEST(Http2ClientSessionTest, PostBodyAndHeadersAreSent) {
    Pair p({}, {}, [](const HttpRequest& req, HttpResponse* resp) {
        resp->setBody(req.header("x-trace") + "|" + req.body() + "|" +
                      req.header("host"));
    });
    HttpRequest r;
    r.setMethod(http::Method::kPost);
    r.setPath("/up");
    r.addHeader("X-Trace", "abc");
    r.addHeader("connection", "keep-alive");  // illegal in h2: dropped
    r.setBody("payload");
    p.client->submit(r, "example.test", "https", p.collect());
    p.pump();
    ASSERT_EQ(p.results.size(), 1u);
    EXPECT_EQ(p.results[0].error, "");
    EXPECT_EQ(p.results[0].response.body(), "abc|payload|example.test");
}

TEST(Http2ClientSessionTest, ManyRequestsMultiplexOnOneSession) {
    Pair p;
    for (int i = 0; i < 50; ++i) {
        p.client->submit(makeGet("/i" + std::to_string(i)), "h", "https",
                         p.collect());
    }
    p.pump();
    ASSERT_EQ(p.results.size(), 50u);
    for (const auto& r : p.results) {
        EXPECT_EQ(r.error, "");
        EXPECT_EQ(r.response.statusCode(), 200);
    }
    EXPECT_EQ(p.handled, 50);
}

TEST(Http2ClientSessionTest, QueueBeyondServerConcurrencyLimit) {
    Http2Session::Limits slim;
    slim.max_concurrent_streams = 2;
    Pair p(slim);
    p.pump();  // learn the server's SETTINGS first
    for (int i = 0; i < 6; ++i) {
        p.client->submit(makeGet("/q" + std::to_string(i)), "h", "https",
                         p.collect());
    }
    EXPECT_EQ(p.client->inFlight(), 2u);  // respects MAX_CONCURRENT_STREAMS
    EXPECT_EQ(p.client->queued(), 4u);
    p.pump();
    ASSERT_EQ(p.results.size(), 6u);
    for (const auto& r : p.results) {
        EXPECT_EQ(r.error, "");
    }
}

TEST(Http2ClientSessionTest, GoAwayMarksUnprocessedStreamsRetryable) {
    Pair p;
    p.pump();
    // Two requests reach the server; it answers stream 1 only, then
    // GOAWAY(last_stream_id=1): stream 3 was not processed.
    int calls = 0;
    p.server.reset();  // replace with a handler that defers stream 3
    Http2Session::Callbacks scb;
    std::vector<int> seen;
    scb.handler = [&](const HttpRequest& req, HttpResponse* resp) {
        ++calls;
        resp->setBody(req.path());
    };
    scb.send = [&p](const char* d, size_t n) { p.s2c.append(d, n); };
    p.server =
        std::make_unique<Http2Session>(std::move(scb), Http2Session::Limits{});
    ASSERT_TRUE(p.server->start());
    p.client = nullptr;
    Http2ClientSession::Callbacks ccb;
    ccb.send = [&p](const char* d, size_t n) { p.c2s.append(d, n); };
    ccb.on_draining = [&p] { ++p.draining_events; };
    p.client = std::make_unique<Http2ClientSession>(
        std::move(ccb), Http2ClientSession::Limits{});
    ASSERT_TRUE(p.client->start());

    p.client->submit(makeGet("/one"), "h", "https", p.collect());
    // Deliver stream 1 and let it complete.
    p.pump();
    ASSERT_EQ(p.results.size(), 1u);
    // Server goes away BEFORE stream 3 arrives: last_stream_id = 1.
    p.server->shutdown(/*immediate=*/true);
    p.client->submit(makeGet("/two"), "h", "https", p.collect());
    // Stream 3 is on the wire to the server, GOAWAY on the wire to the
    // client: the classic race.
    p.pump();

    ASSERT_EQ(p.results.size(), 2u);
    EXPECT_EQ(p.results[0].error, "");
    EXPECT_EQ(p.results[1].error, "");
    EXPECT_TRUE(p.results[1].retryable)
        << "stream above GOAWAY last_stream_id must be retryable: "
        << p.results[1].error;
    // Proves the race happened: the request was on a real stream (3) that
    // reached the server, not rejected locally before being sent. nghttp2's
    // server answers a stream above its GOAWAY with RST_STREAM
    // (REFUSED_STREAM); a server that silently drops it is caught by the
    // GOAWAY rule instead. Both mean "not processed".
    EXPECT_TRUE(p.results[1].error == "stream refused by server" ||
                p.results[1].error == "not processed: server sent GOAWAY")
        << p.results[1].error;
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(p.client->goAwayLastStreamId(), 1);
    EXPECT_FALSE(p.client->canAcceptRequests());
    EXPECT_EQ(p.draining_events, 1);

    // After GOAWAY, new submits fail at once, retryable.
    p.client->submit(makeGet("/three"), "h", "https", p.collect());
    ASSERT_EQ(p.results.size(), 3u);
    EXPECT_TRUE(p.results[2].retryable);
}

TEST(Http2ClientSessionTest, RefusedStreamIsRetrable) {
    // Server caps at 1 open stream; the client floods 3 before it has seen
    // the server's SETTINGS, so streams 3 and 5 are refused.
    Http2Session::Limits slim;
    slim.max_concurrent_streams = 1;
    Pair p(slim);
    for (int i = 0; i < 3; ++i) {
        p.client->submit(makeGet("/r" + std::to_string(i)), "h", "https",
                         p.collect());
    }
    p.pump();
    ASSERT_EQ(p.results.size(), 3u);
    int ok = 0;
    int refused_retryable = 0;
    for (const auto& r : p.results) {
        ok += r.error.empty();
        refused_retryable += !r.error.empty() && r.retryable;
    }
    EXPECT_GE(ok, 1);
    EXPECT_EQ(ok + refused_retryable, 3)
        << "every refused stream must be reported retryable";
}

TEST(Http2ClientSessionTest, StreamIdExhaustionDrainsAndFailsRetryable) {
    Http2ClientSession::Limits clim;
    clim.max_stream_id = 5;  // this session may use streams 1, 3, 5
    Pair p({}, clim);
    for (int i = 0; i < 5; ++i) {
        p.client->submit(makeGet("/e" + std::to_string(i)), "h", "https",
                         p.collect());
        p.pump();
    }
    ASSERT_EQ(p.results.size(), 5u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(p.results[static_cast<size_t>(i)].error, "") << i;
    }
    for (int i = 3; i < 5; ++i) {
        EXPECT_FALSE(p.results[static_cast<size_t>(i)].error.empty());
        EXPECT_TRUE(p.results[static_cast<size_t>(i)].retryable)
            << "an unused request on an exhausted connection is retryable";
    }
    EXPECT_FALSE(p.client->canAcceptRequests());
    EXPECT_EQ(p.draining_events, 1);
    EXPECT_EQ(p.handled, 3);
}

TEST(Http2ClientSessionTest, ConnectionLossInFlightIsNotRetryable) {
    Pair p({}, {}, [](const HttpRequest&, HttpResponse*) {});
    p.pump();
    p.client->submit(makeGet("/x"), "h", "https", p.collect());
    // Send the request but drop the server's reply.
    std::string up;
    up.swap(p.c2s);
    ASSERT_TRUE(p.server->feed(up.data(), up.size()));
    p.s2c.clear();
    p.client->connectionLost("socket closed");
    ASSERT_EQ(p.results.size(), 1u);
    EXPECT_EQ(p.results[0].error, "socket closed");
    EXPECT_FALSE(p.results[0].retryable)
        << "the server may have processed an in-flight request";
}

TEST(Http2ClientSessionTest, ResponseBodyLimitFailsRequst) {
    Http2ClientSession::Limits clim;
    clim.max_response_body = 1024;
    Pair p({}, {}, [](const HttpRequest&, HttpResponse* resp) {
        resp->setBody(std::string(100 * 1024, 'x'));
    });
    p.client->submit(makeGet("/big"), "h", "https", p.collect());
    p.pump();
    ASSERT_EQ(p.results.size(), 1u);
    EXPECT_EQ(p.results[0].error, "response body exceeds limit");
}

TEST(Http2ClientSessionTest, ClientShutdownLetsInFlightFinish) {
    Pair p;
    p.pump();
    p.client->submit(makeGet("/a"), "h", "https", p.collect());
    p.client->shutdown();  // GOAWAY to the server
    p.pump();
    ASSERT_EQ(p.results.size(), 1u);
    EXPECT_EQ(p.results[0].error, "request opened before GOAWAY");
    p.client->submit(makeGet("/b"), "h", "https", p.collect());
    ASSERT_EQ(p.results.size(), 2u);
    EXPECT_TRUE(p.results[1].retryable);
    EXPECT_TRUE(p.client->finished());
}

TEST(Http2ClientSessionTest, StreamAboveGoAwayLineIsRetryableOnClose) {
    // A server that sends GOAWAY(last_stream_id=1) and then just closes the
    // connection, without resetting stream 3. The client must still classify
    // stream 3 as not processed (retryable) and stream 1, cut off mid-flight,
    // as possibly processed (not retryable).
    std::string to_client;
    Http2ClientSession::Callbacks ccb;
    ccb.send = [](const char*, size_t) {};
    Http2ClientSession client(std::move(ccb), {});
    ASSERT_TRUE(client.start());
    std::map<std::string, Http2ClientSession::Result> res;
    auto collectAs = [&](const std::string& key) {
        return [&res, key](Http2ClientSession::Result r) {
            res[key] = std::move(r);
        };
    };
    client.submit(makeGet("/1"), "h", "https", collectAs("s1"));  // stream 1
    client.submit(makeGet("/3"), "h", "https", collectAs("s3"));  // stream 3

    // Hand-built server bytes: empty SETTINGS, then GOAWAY last_stream_id=1.
    const std::string settings("\x00\x00\x00\x04\x00\x00\x00\x00\x00", 9);
    const std::string goaway(
        "\x00\x00\x08\x07\x00\x00\x00\x00"  // len 8, type GOAWAY, sid 0
        "\x00\x00\x00\x01"                  // last_stream_id = 1
        "\x00\x00\x00\x00",                 // NO_ERROR
        17);
    ASSERT_TRUE(client.feed(settings.data(), settings.size()));
    ASSERT_TRUE(client.feed(goaway.data(), goaway.size()));
    EXPECT_EQ(client.goAwayLastStreamId(), 1);
    EXPECT_FALSE(client.canAcceptRequests());

    // Stream 3 is already resolved (nghttp2 closes streams above the GOAWAY
    // line as soon as the frame arrives); stream 1 is still in flight.
    ASSERT_EQ(res.count("s3"), 1u);
    EXPECT_TRUE(res["s3"].retryable) << "stream 3 is above the GOAWAY line";
    EXPECT_EQ(res.count("s1"), 0u);
    EXPECT_EQ(client.inFlight(), 1u);

    client.connectionLost("connection closed");
    ASSERT_EQ(res.count("s1"), 1u);
    EXPECT_FALSE(res["s1"].retryable) << "stream 1 may have been processed";
}
