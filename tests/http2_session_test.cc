// Socket-free tests for http::Http2Session: an nghttp2 CLIENT session is piped
// byte-for-byte into the server session (same harness idea as tls_engine_test).
// Only built when COX_WITH_HTTP2 is ON.

#include "http2_session.h"

#include <nghttp2/nghttp2.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using http::Http2Session;
using http::HttpRequest;
using http::HttpResponse;

namespace {

// Minimal nghttp2 client driven in memory. Collects each stream's response.
class TestClient {
public:
    struct Response {
        int status = 0;
        std::map<std::string, std::string> headers;
        std::string body;
        bool closed = false;
        uint32_t close_code = 0;
    };

    TestClient() {
        nghttp2_session_callbacks* cb = nullptr;
        nghttp2_session_callbacks_new(&cb);
        nghttp2_session_callbacks_set_on_header_callback(cb, &onHeader);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, &onData);
        nghttp2_session_callbacks_set_on_stream_close_callback(cb, &onClose);
        nghttp2_session_callbacks_set_on_frame_recv_callback(cb, &onFrame);
        nghttp2_session_client_new(&session_, cb, this);
        nghttp2_session_callbacks_del(cb);
        nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, nullptr, 0);
    }
    ~TestClient() { nghttp2_session_del(session_); }

    // Submit a request; returns the stream id.
    int32_t request(const std::string& method, const std::string& path,
                    std::vector<std::pair<std::string, std::string>> extra = {},
                    const std::string* body = nullptr) {
        std::vector<std::pair<std::string, std::string>> h = {
            {":method", method},
            {":scheme", "https"},
            {":authority", "localhost"},
            {":path", path}};
        for (auto& e : extra) {
            h.push_back(std::move(e));
        }
        std::vector<nghttp2_nv> nva;
        for (auto& [n, v] : h) {
            nva.push_back({reinterpret_cast<uint8_t*>(n.data()),
                           reinterpret_cast<uint8_t*>(v.data()), n.size(),
                           v.size(), NGHTTP2_NV_FLAG_NONE});
        }
        nghttp2_data_provider prd{};
        if (body != nullptr) {
            body_ = *body;
            body_off_ = 0;
            prd.read_callback = &readBody;
        }
        return nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(),
                                      body != nullptr ? &prd : nullptr, this);
    }

    std::string takeOutput() {
        std::string out;
        const uint8_t* p = nullptr;
        ssize_t n;
        while ((n = nghttp2_session_mem_send(session_, &p)) > 0) {
            out.append(reinterpret_cast<const char*>(p),
                       static_cast<size_t>(n));
        }
        return out;
    }
    void feed(const std::string& bytes) {
        nghttp2_session_mem_recv(session_,
                                 reinterpret_cast<const uint8_t*>(bytes.data()),
                                 bytes.size());
    }

    // Cancel a stream (RST_STREAM(CANCEL)), as a client abandoning a request.
    void reset(int32_t id) {
        nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, id,
                                  NGHTTP2_CANCEL);
    }

    std::map<int32_t, Response> responses;
    // Every GOAWAY received, in order: (last_stream_id, error_code).
    std::vector<std::pair<int32_t, uint32_t>> goaways;
    // Server SETTINGS values received (id -> value).
    std::map<int32_t, uint32_t> settings;

private:
    static int onFrame(nghttp2_session*, const nghttp2_frame* frame, void* u) {
        if (frame->hd.type == NGHTTP2_GOAWAY) {
            self(u)->goaways.emplace_back(frame->goaway.last_stream_id,
                                          frame->goaway.error_code);
        } else if (frame->hd.type == NGHTTP2_SETTINGS &&
                   !(frame->hd.flags & NGHTTP2_FLAG_ACK)) {
            for (size_t i = 0; i < frame->settings.niv; ++i) {
                self(u)->settings[frame->settings.iv[i].settings_id] =
                    frame->settings.iv[i].value;
            }
        }
        return 0;
    }
    static TestClient* self(void* u) { return static_cast<TestClient*>(u); }
    static int onHeader(nghttp2_session*, const nghttp2_frame* frame,
                        const uint8_t* name, size_t namelen,
                        const uint8_t* value, size_t valuelen, uint8_t,
                        void* u) {
        std::string n(reinterpret_cast<const char*>(name), namelen);
        std::string v(reinterpret_cast<const char*>(value), valuelen);
        Response& r = self(u)->responses[frame->hd.stream_id];
        if (n == ":status") {
            r.status = std::stoi(v);
        } else {
            r.headers[n] = v;
        }
        return 0;
    }
    static int onData(nghttp2_session*, uint8_t, int32_t id,
                      const uint8_t* data, size_t len, void* u) {
        self(u)->responses[id].body.append(reinterpret_cast<const char*>(data),
                                           len);
        return 0;
    }
    static int onClose(nghttp2_session*, int32_t id, uint32_t code, void* u) {
        Response& r = self(u)->responses[id];
        r.closed = true;
        r.close_code = code;
        return 0;
    }
    static ssize_t readBody(nghttp2_session*, int32_t, uint8_t* buf,
                            size_t length, uint32_t* flags,
                            nghttp2_data_source*, void* u) {
        TestClient* c = self(u);
        size_t n = std::min(length, c->body_.size() - c->body_off_);
        std::memcpy(buf, c->body_.data() + c->body_off_, n);
        c->body_off_ += n;
        if (c->body_off_ == c->body_.size()) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return static_cast<ssize_t>(n);
    }

    nghttp2_session* session_ = nullptr;
    std::string body_;
    size_t body_off_ = 0;
};

// Server under test plus the byte pipe between it and the client.
struct Harness {
    std::string to_client;
    std::vector<int> rejects;
    int handled = 0;
    std::unique_ptr<Http2Session> server;

    explicit Harness(Http2Session::Handler h, Http2Session::Limits lim = {}) {
        Http2Session::Callbacks cbs;
        cbs.handler = [this, h](const HttpRequest& req, HttpResponse* resp) {
            ++handled;
            h(req, resp);
        };
        cbs.send = [this](const char* d, size_t n) { to_client.append(d, n); };
        cbs.on_reject = [this](int s) { rejects.push_back(s); };
        server = std::make_unique<Http2Session>(std::move(cbs), lim);
        EXPECT_TRUE(server->start());
    }

    // Pump both directions until neither side produces bytes.
    void pump(TestClient& c) {
        for (int i = 0; i < 50; ++i) {
            std::string up = c.takeOutput();
            if (!up.empty()) {
                ASSERT_TRUE(server->feed(up.data(), up.size()))
                    << server->lastError();
            }
            std::string down;
            down.swap(to_client);
            if (!down.empty()) {
                c.feed(down);
            }
            if (up.empty() && down.empty()) {
                return;
            }
        }
    }
};

// Last-stream-ID of every GOAWAY frame in raw server output (frames are a
// 9-byte header: 24-bit length, type, flags, 31-bit stream id; GOAWAY is type
// 0x7 with last_stream_id in its first 4 payload bytes). Lets a test check
// what the server put on the wire independently of client behavior.
std::vector<int32_t> goAwayLastStreamIds(const std::string& wire) {
    std::vector<int32_t> ids;
    size_t i = 0;
    auto u8 = [&](size_t k) { return static_cast<uint8_t>(wire[k]); };
    while (i + 9 <= wire.size()) {
        const size_t len =
            (size_t{u8(i)} << 16) | (size_t{u8(i + 1)} << 8) | u8(i + 2);
        const uint8_t type = u8(i + 3);
        if (i + 9 + len > wire.size()) {
            break;
        }
        if (type == 0x7 && len >= 8) {
            const size_t p = i + 9;
            ids.push_back(static_cast<int32_t>(
                ((uint32_t{u8(p)} & 0x7f) << 24) | (uint32_t{u8(p + 1)} << 16) |
                (uint32_t{u8(p + 2)} << 8) | u8(p + 3)));
        }
        i += 9 + len;
    }
    return ids;
}

void echoHandler(const HttpRequest& req, HttpResponse* resp) {
    resp->setStatus(200);
    resp->setContentType("text/plain");
    resp->setBody(std::string(http::methodToString(req.method())) + " " +
                  req.path() + "?" + req.query() + " " + req.version() +
                  " host=" + req.header("host") + " body=" + req.body());
}

}  // namespace

TEST(Http2SessionTest, SimpleGetReachesHandlerAsHttp2) {
    Harness h(&echoHandler);
    TestClient c;
    int32_t id = c.request("GET", "/hello?x=1");
    h.pump(c);

    ASSERT_TRUE(c.responses.count(id));
    const auto& r = c.responses[id];
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "GET /hello?x=1 HTTP/2 host=localhost body=");
    EXPECT_EQ(r.headers.at("content-type"), "text/plain");
    EXPECT_EQ(r.headers.at("content-length"), std::to_string(r.body.size()));
    EXPECT_TRUE(r.closed);
    EXPECT_EQ(h.server->activeStreams(), 0u);  // stream state released
}

TEST(Http2SessionTest, MultiplexedStreamsOnOneConnection) {
    Harness h(&echoHandler);
    TestClient c;
    std::vector<int32_t> ids;
    for (int i = 0; i < 10; ++i) {
        ids.push_back(c.request("GET", "/item/" + std::to_string(i)));
    }
    h.pump(c);

    EXPECT_EQ(h.handled, 10);
    for (int i = 0; i < 10; ++i) {
        const auto& r = c.responses[ids[static_cast<size_t>(i)]];
        EXPECT_EQ(r.status, 200);
        EXPECT_NE(r.body.find("/item/" + std::to_string(i) + "?"),
                  std::string::npos);
    }
}

TEST(Http2SessionTest, PostBodyIsDelivered) {
    Harness h(&echoHandler);
    TestClient c;
    std::string body = "payload-123";
    int32_t id = c.request("POST", "/up", {}, &body);
    h.pump(c);
    EXPECT_EQ(c.responses[id].status, 200);
    EXPECT_NE(c.responses[id].body.find("body=payload-123"), std::string::npos);
}

TEST(Http2SessionTest, LargeResponseBodySpansManyDataFrames) {
    std::string big(200 * 1024, 'z');  // > one 16KB DATA frame, > 64KB window
    Harness h([&](const HttpRequest&, HttpResponse* resp) {
        resp->setStatus(200);
        resp->setBody(big);
    });
    TestClient c;
    int32_t id = c.request("GET", "/big");
    h.pump(c);
    EXPECT_EQ(c.responses[id].status, 200);
    EXPECT_EQ(c.responses[id].body.size(), big.size());
}

TEST(Http2SessionTest, ConnectionSpecificHeadersAreNotForwarded) {
    Harness h([](const HttpRequest&, HttpResponse* resp) {
        resp->setStatus(204);
        resp->addHeader("Connection", "keep-alive");
        resp->addHeader("X-Custom", "yes");
    });
    TestClient c;
    int32_t id = c.request("GET", "/");
    h.pump(c);
    const auto& r = c.responses[id];
    EXPECT_EQ(r.status, 204);
    EXPECT_EQ(r.headers.count("connection"), 0u);  // illegal in h2
    EXPECT_EQ(r.headers.at("x-custom"), "yes");    // lowercased on the wire
}

TEST(Http2SessionTest, OversizedBodyGets413AndSkipsHandler) {
    Http2Session::Limits lim;
    lim.max_body_bytes = 16;
    Harness h(&echoHandler, lim);
    TestClient c;
    std::string body(1000, 'b');
    int32_t id = c.request("POST", "/up", {}, &body);
    h.pump(c);
    EXPECT_EQ(c.responses[id].status, 413);
    EXPECT_EQ(h.handled, 0);
    ASSERT_EQ(h.rejects.size(), 1u);
    EXPECT_EQ(h.rejects[0], 413);
}

TEST(Http2SessionTest, OversizedHeaderListGets431) {
    Http2Session::Limits lim;
    lim.max_header_list_bytes = 512;
    Harness h(&echoHandler, lim);
    TestClient c;
    int32_t id = c.request("GET", "/", {{"x-big", std::string(2000, 'h')}});
    h.pump(c);
    EXPECT_EQ(c.responses[id].status, 431);
    EXPECT_EQ(h.handled, 0);
}

TEST(Http2SessionTest, UnsupportedMethodGets405) {
    Harness h(&echoHandler);
    TestClient c;
    int32_t id = c.request("TRACE", "/");
    h.pump(c);
    EXPECT_EQ(c.responses[id].status, 405);
    EXPECT_EQ(h.handled, 0);
}

TEST(Http2SessionTest, StreamsBeyondConcurrencyCapAreRefused) {
    // A misbehaving client that opens more streams than the advertised cap
    // before reading the server's SETTINGS (nghttp2 assumes 100 until told).
    // The server must refuse the excess with RST_STREAM(REFUSED_STREAM)
    // instead of running them, and keep serving the admitted ones.
    Http2Session::Limits lim;
    lim.max_concurrent_streams = 2;
    std::string to_client;
    int handled = 0;
    Http2Session::Callbacks cbs;
    cbs.handler = [&](const HttpRequest&, HttpResponse* resp) {
        ++handled;
        resp->setBody("ok");
    };
    cbs.send = [&](const char* d, size_t n) { to_client.append(d, n); };
    // Hold all output so the first two streams stay open while the rest
    // arrive (the cap counts OPEN streams).
    size_t held = 1;
    cbs.pending = [&] { return held; };
    lim.output_high_water = 1;
    Http2Session s(std::move(cbs), lim);
    ASSERT_TRUE(s.start());

    TestClient c;
    for (int i = 0; i < 5; ++i) {
        c.request("GET", "/s" + std::to_string(i));
    }
    std::string up = c.takeOutput();  // preface + 5 HEADERS, no SETTINGS read
    ASSERT_TRUE(s.feed(up.data(), up.size())) << s.lastError();
    held = 0;  // release output
    s.flush();
    c.feed(to_client);

    int ok = 0;
    int refused = 0;
    for (const auto& [id, r] : c.responses) {
        ok += r.status == 200;
        refused += r.closed && r.close_code == NGHTTP2_REFUSED_STREAM;
    }
    EXPECT_EQ(ok, 2);
    EXPECT_EQ(refused, 3);
    EXPECT_EQ(handled, 2);
}

TEST(Http2SessionTest, NonHttp2PrefaceIsAFatalError) {
    Harness h(&echoHandler);
    const std::string h1 = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    EXPECT_FALSE(h.server->feed(h1.data(), h1.size()));
    EXPECT_FALSE(h.server->lastError().empty());
}

TEST(Http2SessionTest, BackpressureStopsFlushAboveHighWater) {
    std::string big(256 * 1024, 'q');
    size_t pending = 0;
    std::string wire;
    Http2Session::Callbacks cbs;
    cbs.handler = [&](const HttpRequest&, HttpResponse* resp) {
        resp->setBody(big);
    };
    cbs.send = [&](const char* d, size_t n) {
        wire.append(d, n);
        pending += n;  // nothing drains until we say so
    };
    cbs.pending = [&] { return pending; };
    Http2Session::Limits lim;
    lim.output_high_water = 8 * 1024;
    Http2Session s(std::move(cbs), lim);
    ASSERT_TRUE(s.start());

    TestClient c;
    c.request("GET", "/big");
    std::string up = c.takeOutput();
    ASSERT_TRUE(s.feed(up.data(), up.size()));
    // Output stalls just above the high-water mark instead of buffering the
    // whole 256KB body.
    EXPECT_LT(wire.size(), 8u * 1024 + 17u * 1024);
    const size_t stalled = wire.size();
    s.flush();
    EXPECT_EQ(wire.size(), stalled);  // still blocked: pending not drained

    pending = 0;  // downstream drained (write-complete)
    s.flush();
    EXPECT_GT(wire.size(), stalled);  // resumed
}

// ---- Configuration, GOAWAY and stream-ID limits ----

TEST(Http2SessionTest, AdvertisesConfiguredSettings) {
    Http2Session::Limits lim;
    lim.max_concurrent_streams = 7;
    lim.max_header_list_bytes = 4096;
    lim.initial_window_size = 1 << 20;
    lim.max_frame_size = 32768;
    Harness h(&echoHandler, lim);
    TestClient c;
    h.pump(c);
    EXPECT_EQ(c.settings[NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS], 7u);
    EXPECT_EQ(c.settings[NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE], 4096u);
    EXPECT_EQ(c.settings[NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE], 1u << 20);
    EXPECT_EQ(c.settings[NGHTTP2_SETTINGS_MAX_FRAME_SIZE], 32768u);
}

TEST(Http2SessionTest, RejectsLimitsOutsideRfcBounds) {
    Http2Session::Limits lim;
    lim.max_frame_size = 1000;  // < 16384
    EXPECT_NE(Http2Session::validate(lim), "");
    lim = {};
    lim.initial_window_size = 1U << 31;  // > 2^31-1
    EXPECT_NE(Http2Session::validate(lim), "");
    lim = {};
    lim.max_stream_id = 1U << 31;
    EXPECT_NE(Http2Session::validate(lim), "");
    EXPECT_EQ(Http2Session::validate(Http2Session::Limits{}), "");

    Http2Session::Limits bad;
    bad.max_frame_size = 1;
    Http2Session::Callbacks cbs;
    cbs.handler = &echoHandler;
    cbs.send = [](const char*, size_t) {};
    Http2Session s(std::move(cbs), bad);
    EXPECT_FALSE(s.start());
    EXPECT_NE(s.lastError().find("max_frame_size"), std::string::npos);
}

TEST(Http2SessionTest, GracefulShutdownIsTwoStepAndFinishesInFlight) {
    Harness h(&echoHandler);
    TestClient c;
    int32_t a = c.request("GET", "/a");
    h.pump(c);
    ASSERT_EQ(c.responses[a].status, 200);

    // Step 1: shutdown notice, last_stream_id = 2^31-1.
    h.server->shutdown();
    EXPECT_TRUE(h.server->goingAway());
    EXPECT_FALSE(h.server->finalGoAwaySent());
    // A request already on the wire when the notice arrives is still served.
    int32_t b = c.request("GET", "/b");
    h.pump(c);
    ASSERT_EQ(c.goaways.size(), 1u);
    EXPECT_EQ(c.goaways[0].first, INT32_MAX);  // 2^31-1
    EXPECT_EQ(c.goaways[0].second, NGHTTP2_NO_ERROR);
    EXPECT_EQ(c.responses[b].status, 200);

    // Step 2: final GOAWAY carries the last stream actually processed.
    // (Checked on the server's output: an nghttp2 client that already holds
    // a GOAWAY and has no open streams stops reading, so it would not report
    // this second frame.)
    std::string before;
    before.swap(h.to_client);
    h.server->shutdown();
    EXPECT_TRUE(h.server->finalGoAwaySent());
    EXPECT_EQ(goAwayLastStreamIds(h.to_client), std::vector<int32_t>{b});
    EXPECT_TRUE(h.server->finished());  // nothing left in flight
}

TEST(Http2SessionTest, StreamOpenedAfterFinalGoAwayIsNotServed) {
    Harness h(&echoHandler);
    TestClient c;
    int32_t a = c.request("GET", "/a");
    h.pump(c);
    h.server->shutdown(/*immediate=*/true);  // final GOAWAY right away
    std::string down;
    down.swap(h.to_client);
    // The client sends a new stream BEFORE reading the GOAWAY (a race every
    // real client can hit): the server must ignore it.
    int32_t late = c.request("GET", "/late");
    std::string up = c.takeOutput();
    ASSERT_TRUE(h.server->feed(up.data(), up.size()));
    c.feed(down);
    c.feed(h.to_client);
    EXPECT_EQ(c.responses[a].status, 200);
    EXPECT_EQ(c.responses[late].status, 0);  // never answered
    EXPECT_EQ(h.handled, 1);
    ASSERT_FALSE(c.goaways.empty());
    EXPECT_EQ(c.goaways.back().first, a);  // client retries > a elsewhere
}

TEST(Http2SessionTest, InFlightStreamCompletesAfterFinalGoAway) {
    // A slow upload: HEADERS arrive, body still pending when GOAWAY goes out.
    Harness h(&echoHandler);
    TestClient c;
    std::string body = "late-body";
    int32_t id = c.request("POST", "/up", {}, &body);
    std::string up = c.takeOutput();
    // Deliver only the preface+SETTINGS+HEADERS part: cut before the DATA
    // frame (DATA is the last 9-byte header + body).
    const size_t data_frame = 9 + body.size();
    ASSERT_GT(up.size(), data_frame);
    ASSERT_TRUE(h.server->feed(up.data(), up.size() - data_frame));
    h.server->shutdown(/*immediate=*/true);
    EXPECT_FALSE(h.server->finished());  // stream `id` still open
    ASSERT_TRUE(h.server->feed(up.data() + up.size() - data_frame, data_frame));
    c.feed(h.to_client);
    EXPECT_EQ(c.responses[id].status, 200);
    EXPECT_NE(c.responses[id].body.find("body=late-body"), std::string::npos);
    EXPECT_TRUE(h.server->finished());
}

TEST(Http2SessionTest, MaxRequestsPerConnectionTriggersGoAway) {
    Http2Session::Limits lim;
    lim.max_requests = 3;
    Harness h(&echoHandler, lim);
    TestClient c;
    for (int i = 0; i < 3; ++i) {
        c.request("GET", "/r" + std::to_string(i));
        h.pump(c);
    }
    EXPECT_EQ(h.server->requestsServed(), 3u);
    EXPECT_TRUE(h.server->goingAway());
    ASSERT_FALSE(c.goaways.empty());
    EXPECT_EQ(c.goaways[0].first, INT32_MAX);  // notice first
    h.to_client.clear();
    h.server->shutdown();                    // owner's step 2
    EXPECT_EQ(h.server->lastStreamId(), 5);  // streams 1, 3, 5
    EXPECT_EQ(goAwayLastStreamIds(h.to_client), std::vector<int32_t>{5});
}

TEST(Http2SessionTest, MaxStreamIdDrainsAndRefusesBeyondIt) {
    Http2Session::Limits lim;
    lim.max_stream_id = 5;  // client may use streams 1, 3, 5
    Harness h(&echoHandler, lim);
    TestClient c;
    std::vector<int32_t> ids;
    for (int i = 0; i < 4; ++i) {  // 1, 3, 5, 7: 7 is over the cap
        ids.push_back(c.request("GET", "/s" + std::to_string(i)));
    }
    h.pump(c);
    EXPECT_EQ(c.responses[ids[0]].status, 200);
    EXPECT_EQ(c.responses[ids[1]].status, 200);
    EXPECT_EQ(c.responses[ids[2]].status, 200);
    // Stream 7 exceeds max_stream_id: not processed. A GOAWAY tells the
    // client so, and it is free to retry it on a new connection.
    EXPECT_EQ(c.responses[ids[3]].status, 0);
    EXPECT_EQ(h.handled, 3);
    ASSERT_FALSE(c.goaways.empty());
    EXPECT_EQ(c.goaways.back().first, 5);
    EXPECT_TRUE(h.server->finished());
}

TEST(Http2SessionTest, ReachingMaxStreamIdStartsGracefulDrain) {
    Http2Session::Limits lim;
    lim.max_stream_id = 3;
    Harness h(&echoHandler, lim);
    TestClient c;
    c.request("GET", "/1");
    h.pump(c);
    EXPECT_FALSE(h.server->goingAway());
    c.request("GET", "/3");  // the last usable ID
    h.pump(c);
    EXPECT_TRUE(h.server->goingAway());  // notice already sent
    ASSERT_FALSE(c.goaways.empty());
    EXPECT_EQ(h.handled, 2);
}

// ---- Async handlers and streamed responses (ServerStream) ----

namespace {
// Server session driven by a stream handler, piped to an nghttp2 client.
struct StreamHarness {
    std::string to_client;
    std::vector<http::ServerStreamPtr> held;  // streams the handler kept
    std::vector<int> statuses;                // on_request statuses
    std::unique_ptr<Http2Session> server;

    explicit StreamHarness(
        std::function<void(const http::ServerStreamPtr&)> handler,
        Http2Session::Limits lim = {}) {
        Http2Session::Callbacks cbs;
        cbs.stream_handler = std::move(handler);
        cbs.send = [this](const char* d, size_t n) { to_client.append(d, n); };
        cbs.on_request = [this](int st, double) { statuses.push_back(st); };
        server = std::make_unique<Http2Session>(std::move(cbs), lim);
        EXPECT_TRUE(server->start());
    }

    void pump(TestClient& c) {
        for (int i = 0; i < 100; ++i) {
            std::string up = c.takeOutput();
            if (!up.empty()) {
                ASSERT_TRUE(server->feed(up.data(), up.size()))
                    << server->lastError();
            }
            std::string down;
            down.swap(to_client);
            if (!down.empty()) {
                c.feed(down);
            }
            if (up.empty() && down.empty()) {
                return;
            }
        }
    }
};
}  // namespace

TEST(Http2StreamingTest, AsyncResponseAfterHandlerReturned) {
    StreamHarness h(
        [&h](const http::ServerStreamPtr& s) { h.held.push_back(s); });
    TestClient c;
    int32_t id = c.request("GET", "/later");
    h.pump(c);
    ASSERT_EQ(h.held.size(), 1u);
    EXPECT_EQ(c.responses[id].status, 0) << "nothing sent yet";
    EXPECT_FALSE(c.responses[id].closed);

    // ...the handler's async work completes:
    HttpResponse resp;
    resp.setStatus(201);
    resp.setBody("done " + h.held[0]->request().path());
    h.held[0]->respond(resp);
    h.pump(c);
    EXPECT_EQ(c.responses[id].status, 201);
    EXPECT_EQ(c.responses[id].body, "done /later");
    EXPECT_TRUE(c.responses[id].closed);
    ASSERT_EQ(h.statuses.size(), 1u);
    EXPECT_EQ(h.statuses[0], 201);
}

TEST(Http2StreamingTest, StreamedBodyWithoutContentLength) {
    StreamHarness h(
        [&h](const http::ServerStreamPtr& s) { h.held.push_back(s); });
    TestClient c;
    int32_t id = c.request("GET", "/events");
    h.pump(c);
    auto s = h.held.at(0);
    s->writeHead(200, {{"content-type", "text/event-stream"}});
    s->write("data: one\n\n");
    h.pump(c);
    // The client has the headers and the first event while the response is
    // still open: that is what makes SSE / long-poll possible.
    EXPECT_EQ(c.responses[id].status, 200);
    EXPECT_EQ(c.responses[id].body, "data: one\n\n");
    EXPECT_FALSE(c.responses[id].closed);
    EXPECT_EQ(c.responses[id].headers.count("content-length"), 0u);

    s->write("data: two\n\n");
    h.pump(c);
    EXPECT_EQ(c.responses[id].body, "data: one\n\ndata: two\n\n");
    s->end();
    h.pump(c);
    EXPECT_TRUE(c.responses[id].closed);
    EXPECT_EQ(c.responses[id].close_code, NGHTTP2_NO_ERROR);
}

TEST(Http2StreamingTest, PendingStreamDoesNotBlockOthers) {
    // Stream /slow is held open by its handler; /fast on the same connection
    // must complete meanwhile (the h2 case where a blocking handler would
    // stall every stream).
    StreamHarness h([&h](const http::ServerStreamPtr& s) {
        if (s->request().path() == "/fast") {
            HttpResponse r;
            r.setBody("fast");
            s->respond(r);
        } else {
            h.held.push_back(s);
        }
    });
    TestClient c;
    int32_t slow = c.request("GET", "/slow");
    int32_t fast = c.request("GET", "/fast");
    h.pump(c);
    EXPECT_EQ(c.responses[fast].body, "fast");
    EXPECT_TRUE(c.responses[fast].closed);
    EXPECT_FALSE(c.responses[slow].closed);
    h.held.at(0)->respond(HttpResponse());
    h.pump(c);
    EXPECT_TRUE(c.responses[slow].closed);
}

TEST(Http2StreamingTest, LargeStreamedBodyRespectsFlowControl) {
    // 1 MB written in 64 KB pieces as the stream drains, i.e. well past the
    // client's 64 KB flow-control window.
    std::string chunk(64 * 1024, 'k');
    int writes_left = 16;
    StreamHarness h([&](const http::ServerStreamPtr& s) {
        h.held.push_back(s);
        s->writeHead(200);
        auto pumpWrites = [&, s]() {
            while (writes_left > 0) {
                --wirtes_left;
                if (!s->write(chunk)) {
                    return;  // wait for onWritable
                }
            }
            s->end();
        };
        s->setOnWritable([pumpWrites] { pumpWrites(); });
        pumpWrites();
    });
    TestClient c;
    int32_t id = c.request("GET", "/big");
    h.pump(c);
    EXPECT_EQ(c.responses[id].body.size(), 16u * chunk.size());
    EXPECT_TRUE(c.responses[id].closed);
}

TEST(Http2StreamingTest, WriteReturnsFalseAboveHighWater) {
    Http2Session::Limits lim;
    lim.stream_high_water = 4096;
    StreamHarness h(
        [&h](const http::ServerStreamPtr& s) { h.held.push_back(s); }, lim);
    TestClient c;
    c.request("GET", "/bp");
    h.pump(c);
    auto s = h.held.at(0);
    s->writeHead(200);
    // Far more than the client's 64 KB window: nghttp2 cannot send it all, so
    // the stream's buffer passes the high-water mark.
    bool ok = true;
    for (int i = 0; i < 40 && ok; ++i) {
        ok = s->write(std::string(4096, 'b'));
    }
    EXPECT_FALSE(ok) << "write() must signal backpressure";
    bool writable = false;
    s->setOnWritable([&] { writable = true; });
    h.pump(c);  // client reads and opens the window
    EXPECT_TRUE(writable) << "onWritable must fire once the stream drains";
    s->end();
    h.pump(c);
}

TEST(Http2StreamingTest, DroppedStreamWithoutResponseGets500) {
    StreamHarness h([](const http::ServerStreamPtr&) {
        // Handler forgets to respond and keeps no reference.
    });
    TestClient c;
    int32_t id = c.request("GET", "/forgot");
    h.pump(c);
    EXPECT_EQ(c.responses[id].status, 500);
    EXPECT_TRUE(c.responses[id].closed);
}

TEST(Http2StreamingTest, DroppedAfterHeadersResetsStream) {
    // The handler starts a streamed response and then drops the stream
    // without end(). Whatever reached the client, it must NOT look like a
    // complete response: the stream ends with an error code, never with a
    // clean END_STREAM.
    for (bool flush_first : {false, true}) {
        SCOPED_TRACE(flush_first ? "headers on the wire before drop"
                                 : "drop inside the handler");
        http::ServerStreamPtr held;
        StreamHarness h([&](const http::ServerStreamPtr& s) {
            s->writeHead(200);
            s->write("partial");
            if (flush_first) {
                held = s;  // keep it past the handler; drop below
            }
        });
        TestClient c;
        int32_t id = c.request("GET", "/half");
        h.pump(c);
        if (flush_first) {
            EXPECT_EQ(c.responses[id].status, 200) << "headers were sent";
            EXPECT_EQ(c.responses[id].body, "partial");
            held.reset();  // abandon mid-body
            h.server->flush();
            h.pump(c);
        }
        EXPECT_TRUE(c.responses[id].closed);
        EXPECT_NE(c.responses[id].close_code, NGHTTP2_NO_ERROR)
            << "a truncated body must not look like a complete one";
    }
}

TEST(Http2StreamingTest, ClientResetAbortsHandler) {
    bool aborted = false;
    StreamHarness h([&](const http::ServerStreamPtr& s) {
        h.held.push_back(s);
        s->onAbort([&] { aborted = true; });
    });
    TestClient c;
    int32_t id = c.request("GET", "/cancel-me");
    h.pump(c);
    ASSERT_FALSE(aborted);
    c.reset(id);  // client cancels: RST_STREAM(CANCEL)
    h.pump(c);
    EXPECT_TRUE(aborted);
    EXPECT_TRUE(h.held.at(0)->aborted());
    // Writing after abort is harmless.
    h.held[0]->writeHead(200);
    EXPECT_FALSE(h.held[0]->write("x"));
    h.held[0]->end();
    h.pump(c);
}

TEST(Http2StreamingTest, SessionDestroyedWhileHandlerHoldsStream) {
    http::ServerStreamPtr kept;
    bool aborted = false;
    {
        StreamHarness h([&](const http::ServerStreamPtr& s) {
            kept = s;
            s->onAbort([&] { aborted = true; });
        });
        TestClient c;
        c.request("GET", "/x");
        h.pump(c);
    }  // connection (session) gone
    EXPECT_TRUE(aborted);
    // The handler finishing later must be a safe no-op.
    kept->respond(HttpResponse());
    kept.reset();
}
