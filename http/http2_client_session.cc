#include "http2_client_session.h"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace http {

namespace {
constexpr uint32_t kProtocolMaxStreamId = (1U << 31) - 1;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Request headers that must not appear in HTTP/2 (RFC 9113 §8.2.2), plus Host
// (carried as :authority) and Content-Length (recomputed from the body).
bool skipHeader(const std::string& lname) {
    return lname == "connection" || lname == "keep-alive" ||
           lname == "proxy-connection" || lname == "transfer-encoding" ||
           lname == "upgrade" || lname == "host" || lname == "te" ||
           lname == "content-length";
}

nghttp2_nv makeNv(const std::string& n, const std::string& v) {
    nghttp2_nv nv;
    nv.name = reinterpret_cast<uint8_t*>(const_cast<char*>(n.data()));
    nv.value = reinterpret_cast<uint8_t*>(const_cast<char*>(v.data()));
    nv.namelen = n.size();
    nv.valuelen = v.size();
    nv.flags = NGHTTP2_NV_FLAG_NONE;
    return nv;
}
}  // namespace

// A request not yet on a stream (queued, or about to be opened).
struct Http2ClientSession::Pending {
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    ResponseCallback cb;
};

// An open stream: the request body being uploaded and the response being
// assembled.
struct Http2ClientSession::Stream {
    int32_t id{0};
    std::string body;
    size_t body_offset{0};
    HttpResponse response;
    std::string response_body;
    bool got_status{false};
    bool too_large{false};
    ResponseCallback cb;
};

struct Http2ClientCallbacks {
    static Http2ClientSession* self(void* u) {
        return static_cast<Http2ClientSession*>(u);
    }

    static int onHeader(nghttp2_session*, const nghttp2_frame* frame,
                        const uint8_t* name, size_t namelen,
                        const uint8_t* value, size_t valuelen, uint8_t,
                        void* u) {
        if (frame->hd.type != NGHTTP2_HEADERS) {
            return 0;
        }
        auto it = self(u)->streams_.find(frame->hd.stream_id);
        if (it == self(u)->streams_.end()) {
            return 0;
        }
        Http2ClientSession::Stream& st = *it->second;
        std::string n(reinterpret_cast<const char*>(name), namelen);
        std::string v(reinterpret_cast<const char*>(value), valuelen);
        if (n == ":status") {
            int code = std::atoi(v.c_str());
            if (code >= 100 && code < 200) {
                return 0;  // interim response (e.g. 103): wait for the final
            }
            st.response.setStatus(code);
            st.got_status = true;
        } else if (!n.empty() && n[0] != ':') {
            st.response.addHeader(std::move(n), std::move(v));
        }
        return 0;
    }

    static int onData(nghttp2_session* session, uint8_t, int32_t stream_id,
                      const uint8_t* data, size_t len, void* u) {
        Http2ClientSession* s = self(u);
        auto it = s->streams_.find(stream_id);
        if (it == s->streams_.end() || it->second->too_large) {
            return 0;
        }
        Http2ClientSession::Stream& st = *it->second;
        if (s->limits_.max_response_body > 0 &&
            st.response_body.size() + len > s->limits_.max_response_body) {
            st.too_large = true;
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id,
                                      NGHTTP2_CANCEL);
            return 0;
        }
        st.response_body.append(reinterpret_cast<const char*>(data), len);
        return 0;
    }

    static int onStreamClose(nghttp2_session*, int32_t stream_id,
                             uint32_t error_code, void* u) {
        Http2ClientSession* s = self(u);
        auto it = s->streams_.find(stream_id);
        if (it == s->streams_.end()) {
            return 0;
        }
        Http2ClientSession::Stream& st = *it->second;
        Http2ClientSession::Result r;
        if (st.too_large) {
            r.error = "response body exceeds limit";
        } else if (error_code == NGHTTP2_NO_ERROR && st.got_status) {
            st.response.setBody(std::move(st.response_body));
            r.response = std::move(st.response);
        } else if (error_code == NGHTTP2_REFUSED_STREAM) {
            r.error = "stream refused by server";
            r.retryable = true;  // RFC 9113 §8.7: not processed
        } else if (s->goaway_last_stream_id_ >= 0 &&
                   stream_id > s->goaway_last_stream_id_) {
            r.error = "not processed: server sent GOAWAY";
            r.retryable = true;  // RFC 9113 §6.8
        } else {
            r.error = std::string("stream reset: ") +
                      nghttp2_http2_strerror(error_code);
        }
        s->complete(stream_id, std::move(r));
        return 0;
    }

    static int onFrameRecv(nghttp2_session*, const nghttp2_frame* frame,
                           void* u) {
        Http2ClientSession* s = self(u);
        if (frame->hd.type == NGHTTP2_GOAWAY) {
            // A GOAWAY can only lower last_stream_id; keep the smallest.
            const int32_t last = frame->goaway.last_stream_id;
            if (s->goaway_last_stream_id_ < 0 ||
                last < s->goaway_last_stream_id_) {
                s->goaway_last_stream_id_ = last;
            }
            s->markDraining();
        } else if (frame->hd.type == NGHTTP2_SETTINGS) {
            s->pumpQueue();  // MAX_CONCURRENT_STREAMS may have grown
        }
        return 0;
    }

    static ssize_t readBody(nghttp2_session*, int32_t stream_id, uint8_t* buf,
                            size_t length, uint32_t* flags,
                            nghttp2_data_source*, void* u) {
        auto it = self(u)->streams_.find(stream_id);
        if (it == self(u)->streams_.end()) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        Http2ClientSession::Stream& st = *it->second;
        const size_t n = std::min(length, st.body.size() - st.body_offset);
        std::memcpy(buf, st.body.data() + st.body_offset, n);
        st.body_offset += n;
        if (st.body_offset == st.body.size()) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return static_cast<ssize_t>(n);
    }
};

Http2ClientSession::Http2ClientSession(Callbacks callbacks, Limits limits)
    : cbs_(std::move(callbacks)), limits_(limits) {}

Http2ClientSession::~Http2ClientSession() {
    failAll("client destroyed", /*queued_retryable=*/true);
    if (session_ != nullptr) {
        nghttp2_session_del(session_);
    }
}

uint32_t Http2ClientSession::maxStreamId() const {
    return limits_.max_stream_id == 0 ? kProtocolMaxStreamId
                                      : limits_.max_stream_id;
}

bool Http2ClientSession::start() {
    nghttp2_session_callbacks* cb = nullptr;
    if (nghttp2_session_callbacks_new(&cb) != 0) {
        last_error_ = "nghttp2_session_callbacks_new failed";
        return false;
    }
    nghttp2_session_callbacks_set_on_header_callback(
        cb, &Http2ClientCallbacks::onHeader);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        cb, &Http2ClientCallbacks::onData);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        cb, &Http2ClientCallbacks::onStreamClose);
    nghttp2_session_callbacks_set_on_frame_recv_callback(
        cb, &Http2ClientCallbacks::onFrameRecv);
    int rv = nghttp2_session_client_new(&session_, cb, this);
    nghttp2_session_callbacks_del(cb);
    if (rv != 0) {
        session_ = nullptr;
        last_error_ =
            std::string("nghttp2_session_client_new: ") + nghttp2_strerror(rv);
        return false;
    }
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},  // Cox never accepts server push
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, limits_.initial_window_size},
    };
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv,
                            sizeof(iv) / sizeof(iv[0]));
    flush();
    return true;
}

bool Http2ClientSession::canAcceptRequests() const {
    return session_ != nullptr && !draining_ && !dead_;
}

bool Http2ClientSession::hasFreeStream() const {
    if (!canAcceptRequests() || !queue_.empty()) {
        return false;
    }
    return streams_.size() <
           nghttp2_session_get_remote_settings(
               session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
}

bool Http2ClientSession::finished() const {
    if (session_ == nullptr || dead_) {
        return true;
    }
    return streams_.empty() && queue_.empty() &&
           !nghttp2_session_want_write(session_) &&
           (draining_ || !nghttp2_session_want_read(session_));
}

void Http2ClientSession::submit(const HttpRequest& req,
                                const std::string& authority,
                                const std::string& scheme,
                                ResponseCallback cb) {
    if (!canAcceptRequests()) {
        cb(Result{HttpResponse(), "connection is not accepting requests",
                  /*retryable=*/true});
        return;
    }
    auto p = std::make_unique<Pending>();
    std::string path = req.path().empty() ? "/" : req.path();
    if (!req.query().empty()) {
        path += "?" + req.query();
    }
    p->headers = {{":method", methodToString(req.method())},
                  {":scheme", scheme},
                  {":authority", authority},
                  {":path", path}};
    for (const auto& [k, v] : req.headers()) {
        std::string lk = lower(k);
        if (!skipHeader(lk)) {
            p->headers.emplace_back(std::move(lk), v);
        }
    }
    if (!req.body().empty()) {
        p->headers.emplace_back("content-length",
                                std::to_string(req.body().size()));
    }
    p->body = req.body();
    p->cb = std::move(cb);

    if (queue_.size() >= limits_.max_queued) {
        p->cb(Result{HttpResponse(), "request queue full", true});
        return;
    }
    queue_.push_back(std::move(p));
    pumpQueue();
    flush();
}

void Http2ClientSession::pumpQueue() {
    if (session_ == nullptr || dead_) {
        return;
    }
    while (!queue_.empty() && !draining_) {
        const uint32_t max_concurrent = nghttp2_session_get_remote_settings(
            session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
        if (streams_.size() >= max_concurrent) {
            return;  // wait for a stream to close
        }
        std::unique_ptr<Pending> p = std::move(queue_.front());
        queue_.pop_front();
        if (!openStream(std::move(p))) {
            return;
        }
    }
}

bool Http2ClientSession::openStream(std::unique_ptr<Pending> p) {
    // Client stream IDs are odd and strictly increasing; once the next one
    // would exceed the limit this connection is used up (nothing can reset
    // the counter), so the request goes to a new connection instead.
    const uint32_t next = nghttp2_session_get_next_stream_id(session_);
    if (next > maxStreamId()) {
        queue_.push_front(std::move(p));
        markDraining();  // fails the queue as retryable
        return false;
    }
    std::vector<nghttp2_nv> nva;
    nva.reserve(p->headers.size());
    for (const auto& [n, v] : p->headers) {
        nva.push_back(makeNv(n, v));
    }
    auto st = std::make_unique<Stream>();
    st->body = std::move(p->body);
    st->cb = std::move(p->cb);
    nghttp2_data_provider prd;
    prd.source.ptr = nullptr;
    prd.read_callback = &Http2ClientCallbacks::readBody;
    const bool has_body = !st->body.empty();
    const int32_t id =
        nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(),
                               has_body ? &prd : nullptr, nullptr);
    if (id < 0) {
        const bool retryable = id == NGHTTP2_ERR_STREAM_ID_NOT_AVAILABLE ||
                               id == NGHTTP2_ERR_START_STREAM_NOT_ALLOWED;
        st->cb(
            Result{HttpResponse(),
                   std::string("cannot open stream: ") + nghttp2_strerror(id),
                   retryable});
        if (retryable) {
            markDraining();
        }
        return false;
    }
    st->id = id;
    streams_[id] = std::move(st);
    return true;
}

void Http2ClientSession::complete(int32_t stream_id, Result result) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return;
    }
    ResponseCallback cb = std::move(it->second->cb);
    streams_.erase(it);
    if (cb) {
        cb(std::move(result));
    }
    pumpQueue();  // a slot freed up
}

void Http2ClientSession::markDraining() {
    if (draining_) {
        return;
    }
    draining_ = true;
    // Requests that never got a stream were not seen by the server.
    while (!queue_.empty()) {
        std::unique_ptr<Pending> p = std::move(queue_.front());
        queue_.pop_front();
        p->cb(Result{HttpResponse(), "connection draining", true});
    }
    if (cbs_.on_draining) {
        cbs_.on_draining();
    }
}

void Http2ClientSession::failAll(const std::string& why,
                                 bool queued_retryable) {
    std::map<int32_t, std::unique_ptr<Stream>> streams;
    streams.swap(streams_);
    std::deque<std::unique_ptr<Pending>> queue;
    queue.swap(queue_);
    for (auto& [id, st] : streams) {
        if (!st->cb) {
            continue;
        }
        // Above the GOAWAY line the server promised not to process it.
        const bool retryable =
            goaway_last_stream_id_ >= 0 && id > goaway_last_stream_id_;
        st->cb(Result{HttpResponse(), why, retryable});
    }
    for (auto& p : queue) {
        p->cb(Result{HttpResponse(), why, queued_retryable});
    }
}

bool Http2ClientSession::feed(const char* data, size_t len) {
    if (session_ == nullptr || dead_) {
        return false;
    }
    ssize_t rv = nghttp2_session_mem_recv(
        session_, reinterpret_cast<const uint8_t*>(data), len);
    if (rv < 0) {
        last_error_ = std::string("nghttp2_session_mem_recv: ") +
                      nghttp2_strerror(static_cast<int>(rv));
        dead_ = true;
        failAll("HTTP/2 protocol error: " + last_error_, true);
        return false;
    }
    flush();
    return true;
}

void Http2ClientSession::flush() {
    if (session_ == nullptr || flushing_) {
        return;
    }
    flushing_ = true;
    for (;;) {
        if (cbs_.pending && cbs_.pending() >= limits_.output_high_water) {
            break;
        }
        const uint8_t* out = nullptr;
        ssize_t n = nghttp2_session_mem_send(session_, &out);
        if (n <= 0) {
            break;
        }
        cbs_.send(reinterpret_cast<const char*>(out), static_cast<size_t>(n));
    }
    flushing_ = false;
}

void Http2ClientSession::connectionLost(const std::string& why) {
    if (dead_) {
        return;
    }
    dead_ = true;
    failAll(why, /*queued_retryable=*/true);
}

void Http2ClientSession::shutdown() {
    if (session_ == nullptr || dead_) {
        return;
    }
    if (!draining_) {
        nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE,
                              /*last_stream_id=*/0, NGHTTP2_NO_ERROR, nullptr,
                              0);
        markDraining();
    }
    flush();
}

}  // namespace http
