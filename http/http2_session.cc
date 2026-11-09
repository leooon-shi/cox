#include "http2_session.h"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace http {

// Per-stream state: the request being assembled and, once the handler has
// run, the response body nghttp2 pulls through the data provider.
struct Http2Session::Stream {
    int32_t id{0};
    HttpRequest request;
    std::string method;
    std::string path;
    bool has_authority_or_host{false};
    size_t header_bytes{0};
    size_t body_bytes{0};
    // Set when the request is rejected or answered; further request headers /
    // body data for this stream are ignored.
    bool done{false};
    bool header_list_too_large{false};
    bool body_too_large{false};
    // Rejected while the peer may still be uploading: reset the stream once
    // the response has been sent.
    bool reset_after_response{false};
    // Response body bytes not yet handed to nghttp2. For a buffered response
    // it is the whole body; for a streamed one it grows with write().
    std::string response_body;
    size_t body_offset{0};
    // Streamed response: more data may still come (end() not called yet).
    // The data provider returns DEFERRED while the buffer is empty and this
    // is true.
    bool body_open{false};
    // nghttp2 paused the provider (DEFERRED); resume_data on the next write.
    bool deferred{false};
    // The handler's view of this exchange (async/streaming handlers). Weak:
    // the handler owns it; the session only needs to notify it.
    std::weak_ptr<ServerStream> server_stream;
    int status{0};
    std::chrono::steady_clock::time_point started;
};

namespace {
std::string lower(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// HTTP/1.1 connection-specific headers are forbidden in HTTP/2 (RFC 9113
// §8.2.2); never copy them from an HttpResponse into h2 HEADERS.
bool isConnectionSpecific(const std::string& lname) {
    return lname == "connection" || lname == "keep-alive" ||
           lname == "proxy-connection" || lname == "transfer-encoding" ||
           lname == "upgrade" || lname == "content-length";
}

nghttp2_nv makeNv(const std::string& name, const std::string& value) {
    nghttp2_nv nv;
    nv.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
    nv.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
    nv.namelen = name.size();
    nv.valuelen = value.size();
    nv.flags = NGHTTP2_NV_FLAG_NONE;
    return nv;
}
}  // namespace

// Connects one ServerStream to its HTTP/2 stream. The handler may keep the
// ServerStream after the connection (and this session) is gone, so every
// call first checks the session's liveness flag; it never holds a raw
// pointer it has not just validated.
class Http2Session::H2Sink : public ServerStream::Sink {
public:
    H2Sink(Http2Session* session, std::shared_ptr<bool> alive, int32_t id,
           bool head_request)
        : session_(session),
          alive_(std::move(alive)),
          id_(id),
          head_request_(head_request) {}

    void sendHead(int status, const ServerStream::Headers& headers,
                  bool has_body, bool) override {
        if (!live()) {
            return;
        }
        head_sent_ = true;
        session_->streamHead(id_, status, headers, has_body, head_request_);
    }
    size_t sendData(std::string_view data) override {
        if (!live() || head_request_) {
            return 0;
        }
        return session_->streamData(id_, data);
    }
    void sendEnd() override {
        if (live()) {
            session_->streamEnd(id_);
        }
    }
    void abandoned() override {
        if (live()) {
            session_->streamAbandoned(id_, head_sent_);
        }
    }
    reactor::EventLoop* loop() const override {
        return live() ? session_->cbs_.loop : nullptr;
    }

private:
    bool live() const { return *alive_ && session_->streamExists(id_); }

    Http2Session* session_;
    std::shared_ptr<bool> alive_;
    int32_t id_;
    bool head_request_;
    bool head_sent_{false};
};

// C-ABI trampolines. user_data is always the Http2Session.
struct Http2SessionCallbacks {
    static Http2Session* self(void* user_data) {
        return static_cast<Http2Session*>(user_data);
    }

    static int onBeginHeaders(nghttp2_session* session,
                              const nghttp2_frame* frame, void* user_data) {
        if (frame->hd.type != NGHTTP2_HEADERS ||
            frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
            return 0;
        }
        Http2Session* s = self(user_data);
        const int32_t id = frame->hd.stream_id;
        // Stream-ID cap: refuse streams beyond it (RFC 9113 §8.7: a
        // REFUSED_STREAM request was not processed and is safe to retry on a
        // new connection), and start draining once it is reached.
        if (static_cast<uint32_t>(id) > s->effectiveMaxStreamId()) {
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, id,
                                      NGHTTP2_REFUSED_STREAM);
            s->shutdown(/*immediate=*/true);
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        if (id > s->last_stream_id_) {
            s->last_stream_id_ = id;
        }
        auto st = std::make_unique<Http2Session::Stream>();
        st->id = id;
        st->request.setVersion("HTTP/2");
        s->streams_[id] = std::move(st);
        // Last stream ID this connection may use (the client cannot go
        // higher than max_stream_id in its odd ID space): drain after it.
        if (static_cast<uint32_t>(id) + 2 > s->effectiveMaxStreamId() &&
            s->goaway_state_ == Http2Session::GoAway::kNone) {
            s->shutdown();  // notice only; see countRequest()
        }
        return 0;
    }

    static int onHeader(nghttp2_session*, const nghttp2_frame* frame,
                        const uint8_t* name, size_t namelen,
                        const uint8_t* value, size_t valuelen, uint8_t,
                        void* user_data) {
        if (frame->hd.type != NGHTTP2_HEADERS) {
            return 0;
        }
        Http2Session* s = self(user_data);
        Http2Session::Stream* st = s->findStream(frame->hd.stream_id);
        if (st == nullptr || st->done) {
            return 0;
        }
        // Same accounting as RFC 9113 §6.5.2 SETTINGS_MAX_HEADER_LIST_SIZE:
        // name + value + 32 bytes of overhead per field.
        st->header_bytes += namelen + valuelen + 32;
        if (st->header_bytes > s->limits_.max_header_list_bytes) {
            st->header_list_too_large = true;
            return 0;  // keep decoding (HPACK state) but drop the field
        }
        std::string n(reinterpret_cast<const char*>(name), namelen);
        std::string v(reinterpret_cast<const char*>(value), valuelen);
        if (!n.empty() && n[0] == ':') {
            if (n == ":method") {
                st->method = v;
            } else if (n == ":path") {
                st->path = v;
            } else if (n == ":authority") {
                st->has_authority_or_host = true;
                st->request.addHeader("host", v);
            }
            return 0;  // :scheme and others need no mapping
        }
        if (lower(name, namelen) == "host") {
            st->has_authority_or_host = true;
        }
        st->request.addHeader(std::move(n), std::move(v));
        return 0;
    }

    static int onDataChunk(nghttp2_session*, uint8_t, int32_t stream_id,
                           const uint8_t* data, size_t len, void* user_data) {
        Http2Session* s = self(user_data);
        Http2Session::Stream* st = s->findStream(stream_id);
        if (st == nullptr || st->done) {
            return 0;  // body of a rejected/answered stream: discard
        }
        st->body_bytes += len;
        if (s->limits_.max_body_bytes > 0 &&
            st->body_bytes > s->limits_.max_body_bytes) {
            st->body_too_large = true;
            s->reject(st, 413);
            return 0;
        }
        st->request.appendBody(reinterpret_cast<const char*>(data), len);
        return 0;
    }

    static int onFrameRecv(nghttp2_session*, const nghttp2_frame* frame,
                           void* user_data) {
        Http2Session* s = self(user_data);
        const bool end_stream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM);
        switch (frame->hd.type) {
            case NGHTTP2_HEADERS:
            case NGHTTP2_DATA: {
                Http2Session::Stream* st = s->findStream(frame->hd.stream_id);
                if (st == nullptr || st->done) {
                    return 0;
                }
                if (st->header_list_too_large) {
                    s->reject(st, 431);
                    return 0;
                }
                if (end_stream) {
                    s->dispatch(st);
                }
                return 0;
            }
            default:
                return 0;
        }
    }

    static int onStreamClose(nghttp2_session*, int32_t stream_id,
                             uint32_t error_code, void* user_data) {
        Http2Session* s = self(user_data);
        Http2Session::Stream* st = s->findStream(stream_id);
        if (st != nullptr) {
            // Closed before the handler finished: the client reset it, or
            // the connection is going away. Tell an async handler to stop.
            if (auto ss = st->server_stream.lock()) {
                if (!ss->finished()) {
                    ss->notifyAbort();
                }
            }
            (void)error_code;
        }
        s->streams_.erase(stream_id);
        return 0;
    }

    static int onFrameSend(nghttp2_session* session, const nghttp2_frame* frame,
                           void* user_data) {
        Http2Session* s = self(user_data);
        if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
            return 0;
        }
        Http2Session::Stream* st = s->findStream(frame->hd.stream_id);
        if (st != nullptr && st->reset_after_response &&
            nghttp2_session_get_stream_remote_close(session, st->id) == 0) {
            st->reset_after_response = false;
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, st->id,
                                      NGHTTP2_NO_ERROR);
        }
        return 0;
    }

    static ssize_t readBody(nghttp2_session*, int32_t stream_id, uint8_t* buf,
                            size_t length, uint32_t* data_flags,
                            nghttp2_data_source*, void* user_data) {
        Http2Session* s = self(user_data);
        Http2Session::Stream* st = s->findStream(stream_id);
        if (st == nullptr) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        const size_t left = st->response_body.size() - st->body_offset;
        const size_t n = std::min(left, length);
        if (n > 0) {
            std::memcpy(buf, st->response_body.data() + st->body_offset, n);
            st->body_offset += n;
        }
        if (st->body_offset == st->response_body.size()) {
            // Drained: release the memory, then either finish or wait.
            st->response_body.clear();
            st->body_offset = 0;
            if (!st->body_open) {
                *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            } else if (n == 0) {
                st->deferred = true;
                return NGHTTP2_ERR_DEFERRED;  // resumed by the next write()
            }
        }
        if (n > 0) {
            // Not here: readBody runs inside nghttp2_session_mem_send, and
            // an onWritable callback would re-enter the session. flush()
            // notifies writable streams once mem_send has returned.
            s->writable_pending_ = true;
        }
        return static_cast<ssize_t>(n);
    }
};

Http2Session::Http2Session(Callbacks callbacks, Limits limits)
    : cbs_(std::move(callbacks)),
      limits_(limits),
      alive_(std::make_shared<bool>(true)) {}

Http2Session::~Http2Session() {
    *alive_ = false;  // ServerStreams that outlive us become no-ops
    // Streams still open when the connection goes away: tell their handlers.
    std::vector<std::shared_ptr<ServerStream>> open;
    for (auto& [id, st] : streams_) {
        if (auto ss = st->server_stream.lock()) {
            open.push_back(ss);
        }
    }
    for (auto& ss : open) {
        ss->notifyAbort();
    }
    if (session_ != nullptr) {
        nghttp2_session_del(session_);
    }
}

namespace {
constexpr uint32_t kMaxStreamIdProtocol = (1U << 31) - 1;  // RFC 9113 §5.1.1
constexpr uint32_t kMaxWindow = (1U << 31) - 1;            // RFC 9113 §6.9.1
constexpr uint32_t kMinFrameSize = 16384;                  // RFC 9113 §6.5.2
constexpr uint32_t kMaxFrameSize = (1U << 24) - 1;
}  // namespace

std::string Http2Session::validate(const Limits& l) {
    if (l.initial_window_size > kMaxWindow) {
        return "initial_window_size must be <= 2^31-1";
    }
    if (l.connection_window_size > kMaxWindow) {
        return "connection_window_size must be <= 2^31-1";
    }
    if (l.max_frame_size < kMinFrameSize || l.max_frame_size > kMaxFrameSize) {
        return "max_frame_size must be in [16384, 16777215]";
    }
    if (l.max_stream_id > kMaxStreamIdProtocol) {
        return "max_stream_id must be <= 2^31-1 (0 = protocol maximum)";
    }
    if (l.max_header_list_bytes > UINT32_MAX) {
        return "max_header_list_bytes must fit in 32 bits";
    }
    return std::string();
}

uint32_t Http2Session::effectiveMaxStreamId() const {
    return limits_.max_stream_id == 0 ? kMaxStreamIdProtocol
                                      : limits_.max_stream_id;
}

void Http2Session::countRequest() {
    ++requests_served_;
    // Only ever START the drain here (the shutdown notice). Several requests
    // can complete inside one feed(); a second call must not turn into the
    // final GOAWAY, whose last_stream_id would then name streams whose
    // responses are still queued behind it. The final GOAWAY is the owner's
    // job (HttpServer's notice timer), after the notice reached the peer.
    if (limits_.max_requests != 0 && requests_served_ >= limits_.max_requests &&
        goaway_state_ == GoAway::kNone) {
        shutdown();
    }
}

void Http2Session::shutdown(bool immediate) {
    if (session_ == nullptr || goaway_state_ == GoAway::kFinalSent) {
        return;
    }
    if (immediate || goaway_state_ != GoAway::kNone) {
        // Final GOAWAY: last_stream_id = the highest stream we have seen.
        // Streams above it are refused by nghttp2 from now on; streams at or
        // below it finish normally.
        nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, last_stream_id_,
                              NGHTTP2_NO_ERROR, nullptr, 0);
        goaway_state_ = GoAway::kFinalSent;
    } else {
        // Step 1: the notice. Step 2 is the next shutdown() call, which the
        // owner makes after roughly one round trip (HttpServer uses a
        // timer), or on its own if a stream/request limit is hit again.
        nghttp2_submit_shutdown_notice(session_);
        goaway_state_ = GoAway::kNoticeQueued;
    }
    flush();
}

bool Http2Session::start() {
    last_error_ = validate(limits_);
    if (!last_error_.empty()) {
        return false;
    }
    nghttp2_session_callbacks* cb = nullptr;
    if (nghttp2_session_callbacks_new(&cb) != 0) {
        last_error_ = "nghttp2_session_callback_new failed";
        return false;
    }
    nghttp2_session_callbacks_set_on_begin_headers_callback(
        cb, &Http2SessionCallbacks::onBeginHeaders);
    nghttp2_session_callbacks_set_on_header_callback(
        cb, &Http2SessionCallbacks::onHeader);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        cb, &Http2SessionCallbacks::onDataChunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(
        cb, &Http2SessionCallbacks::onFrameRecv);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        cb, &Http2SessionCallbacks::onStreamClose);
    nghttp2_session_callbacks_set_on_frame_send_callback(
        cb, &Http2SessionCallbacks::onFrameSend);

    nghttp2_option* opt = nullptr;
    nghttp2_option_new(&opt);
    // Inbound header-list size is enforced by onHeader's accounting (431)
    // against the SETTINGS_MAX_HEADER_LIST_SIZE we advertise below. nghttp2's
    // defaults already rate-limit RST_STREAM floods (CVE-2023-44487).

    int rv = nghttp2_session_server_new2(&session_, cb, this, opt);
    nghttp2_option_del(opt);
    nghttp2_session_callbacks_del(cb);
    if (rv != 0) {
        session_ = nullptr;
        last_error_ =
            std::string("nghttp2_session_server_new2: ") + nghttp2_strerror(rv);
        return false;
    }

    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
         limits_.max_concurrent_streams},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
         static_cast<uint32_t>(limits_.max_header_list_bytes)},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, limits_.initial_window_size},
        {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, limits_.max_frame_size},
    };
    rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv,
                                 sizeof(iv) / sizeof(iv[0]));
    if (rv != 0) {
        last_error_ =
            std::string("nghttp2_submit_settings: ") + nghttp2_strerror(rv);
        return false;
    }
    if (limits_.connection_window_size != 0) {
        // Stream 0 window is not a SETTING: raise (or lower) it explicitly.
        rv = nghttp2_session_set_local_window_size(
            session_, NGHTTP2_FLAG_NONE, 0,
            static_cast<int32_t>(limits_.connection_window_size));
        if (rv != 0) {
            last_error_ =
                std::string("nghttp2_session_set_local_window_size: ") +
                nghttp2_strerror(rv);
            return false;
        }
    }
    flush();
    return true;
}

bool Http2Session::feed(const char* data, size_t len) {
    if (session_ == nullptr) {
        last_error_ = "session not started";
        return false;
    }
    // While nghttp2 is inside mem_recv (and so inside our callbacks, which
    // may run handlers that respond at once), flush() must not call
    // mem_send: nghttp2 is not re-entrant that way. The flush below, after
    // mem_recv returns, sends everything that was submitted.
    receiving_ = true;
    ssize_t rv = nghttp2_session_mem_recv(
        session_, reinterpret_cast<const uint8_t*>(data), len);
    receiving_ = false;
    if (rv < 0) {
        last_error_ = std::string("nghttp2_session_mem_recv: ") +
                      nghttp2_strerror(static_cast<int>(rv));
        // Best effort: tell the peer why before the caller closes.
        nghttp2_session_terminate_session(session_, NGHTTP2_PROTOCOL_ERROR);
        flush();
        return false;
    }
    flush();
    return true;
}

void Http2Session::flush() {
    if (session_ == nullptr || flushing_ || receiving_) {
        return;
    }
    flushing_ = true;
    for (;;) {
        if (cbs_.pending && cbs_.pending() >= limits_.output_high_water) {
            break;  // downstream full: resume on write-complete
        }
        const uint8_t* out = nullptr;
        ssize_t n = nghttp2_session_mem_send(session_, &out);
        if (n <= 0) {
            break;  // nothing to send (or an internal error)
        }
        cbs_.send(reinterpret_cast<const char*>(out), static_cast<size_t>(n));
    }
    flushing_ = false;
    if (writable_pending_) {
        writable_pending_ = false;
        notifyWritableStreams();  // may write, which flushes again
    }
}

bool Http2Session::finished() const {
    return session_ == nullptr || (!nghttp2_session_want_read(session_) &&
                                   !nghttp2_session_want_write(session_));
}

Http2Session::Stream* Http2Session::findStream(int32_t stream_id) {
    auto it = streams_.find(stream_id);
    return it == streams_.end() ? nullptr : it->second.get();
}

void Http2Session::reject(Stream* s, int status) {
    s->done = true;
    HttpResponse resp;
    resp.setStatus(status);
    submitResponse(s, resp, /*no_body=*/false);
    countRequest();
    if (cbs_.on_reject) {
        cbs_.on_reject(status);
    }
    if (cbs_.on_request) {
        cbs_.on_request(status, 0.0);
    }
    // Stop a peer that is still uploading (RFC 9113 §8.1: respond, then
    // RST_STREAM(NO_ERROR)). The reset is submitted from onFrameSend once the
    // response's END_STREAM frame is on the wire; resetting now would discard
    // the queued error response.
    s->reset_after_response = true;
}

void Http2Session::dispatch(Stream* s) {
    s->done = true;
    Method m = methodFromString(s->method);
    if (m == Method::kInvalid || s->path.empty() || s->path[0] != '/') {
        // nghttp2 already validates pseudo-headers; this catches methods the
        // Cox request model does not represent (e.g. CONNECT, TRACE).
        reject(s, m == Method::kInvalid ? 405 : 400);
        return;
    }
    s->request.setMethod(m);
    const auto q = s->path.find('?');
    s->request.setPath(s->path.substr(0, q));
    if (q != std::string::npos) {
        s->request.setQuery(s->path.substr(q + 1));
    }
    s->started = std::chrono::steady_clock::now();

    if (cbs_.stream_handler) {
        // Async / streaming: the handler gets a ServerStream and may answer
        // now or later. Request accounting happens when it finishes.
        auto sink =
            std::make_shared<H2Sink>(this, alive_, s->id, m == Method::kHead);
        auto ss = std::make_shared<ServerStream>(std::move(s->request), sink,
                                                 limits_.stream_high_water);
        s->server_stream = ss;
        cbs_.stream_handler(ss);
        // `ss` going out of scope here is the handler's decision point: if
        // it kept no reference and did not answer, ~ServerStream sends 500.
        return;
    }

    HttpResponse resp(/*keep_alive=*/true);
    if (cbs_.handler) {
        cbs_.handler(s->request, &resp);
    } else {
        resp.setStatus(404);
        resp.setBody("No handler");
    }
    double secs = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - s->started)
                      .count();
    submitResponse(s, resp, /*no_body=*/m == Method::kHead);
    if (cbs_.on_request) {
        cbs_.on_request(resp.statusCode(), secs);
    }
    countRequest();
}

// ---- Streaming response plumbing (used by H2Sink) ----

void Http2Session::streamHead(int32_t id, int status,
                              const ServerStream::Headers& headers,
                              bool has_body, bool no_body_method) {
    Stream* s = findStream(id);
    if (s == nullptr) {
        return;
    }
    s->status = status;
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back(":status", std::to_string(status));
    for (const auto& [key, value] : headers) {
        std::string lname = key;
        std::transform(lname.begin(), lname.end(), lname.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (isConnectionSpecific(lname) && lname != "content-length") {
            continue;  // connection-specific; content-length is fine in h2
        }
        fields.emplace_back(std::move(lname), value);
    }
    std::vector<nghttp2_nv> nva;
    nva.reserve(fields.size());
    for (const auto& [n, v] : fields) {
        nva.push_back(makeNv(n, v));
    }
    s->body_open = has_body && !no_body_method;
    nghttp2_data_provider prd;
    prd.source.ptr = nullptr;
    prd.read_callback = &Http2SessionCallbacks::readBody;
    nghttp2_submit_response(session_, id, nva.data(), nva.size(),
                            s->body_open ? &prd : nullptr);
    flush();
}

size_t Http2Session::streamData(int32_t id, std::string_view data) {
    Stream* s = findStream(id);
    if (s == nullptr || !s->body_open) {
        return 0;
    }
    if (s->body_offset > 0 && s->body_offset * 2 > s->response_body.size()) {
        // Compact once more than half the buffer has been sent.
        s->response_body.erase(0, s->body_offset);
        s->body_offset = 0;
    }
    s->response_body.append(data.data(), data.size());
    if (s->deferred) {
        s->deferred = false;
        nghttp2_session_resume_data(session_, id);
    }
    flush();
    s = findStream(id);  // flush may have closed it
    return s == nullptr ? 0 : s->response_body.size() - s->body_offset;
}

void Http2Session::streamEnd(int32_t id) {
    Stream* s = findStream(id);
    if (s == nullptr) {
        return;
    }
    const int status = s->status;
    const double secs = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - s->started)
                            .count();
    if (s->body_open) {
        s->body_open = false;
        if (s->deferred) {
            s->deferred = false;
            nghttp2_session_resume_data(session_, id);  // -> EOF
        }
    }
    if (cbs_.on_request) {
        cbs_.on_request(status, secs);
    }
    countRequest();
    flush();
}

void Http2Session::streamAbandoned(int32_t id, bool head_sent) {
    Stream* s = findStream(id);
    if (s == nullptr) {
        return;
    }
    if (!head_sent) {
        // The handler dropped the stream without answering: 500, never a
        // hung client.
        s->server_stream.reset();
        HttpResponse resp;
        resp.setStatus(500);
        submitResponse(s, resp, /*no_body=*/false);
        if (cbs_.on_request) {
            cbs_.on_request(500, 0.0);
        }
        countRequest();
    } else {
        // Headers are out but the body was never finished: the response is
        // truncated. Reset the stream so the client sees an error, not a
        // silently short body.
        nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, id,
                                  NGHTTP2_INTERNAL_ERROR);
    }
    flush();
}

void Http2Session::notifyWritableStreams() {
    // Snapshot (id, buffered) first: an onWritable callback may write (and
    // re-flush), end or abandon streams, which mutates streams_.
    std::vector<std::pair<std::weak_ptr<ServerStream>, size_t>> ready;
    for (auto& [id, st] : streams_) {
        if (!st->server_stream.expired()) {
            ready.emplace_back(st->server_stream,
                               st->response_body.size() - st->body_offset);
        }
    }
    for (auto& [weak, buffered] : ready) {
        if (auto ss = weak.lock()) {
            ss->notifyWritable(buffered);
        }
    }
}

bool Http2Session::streamExists(int32_t id) const {
    return streams_.count(id) != 0;
}

void Http2Session::submitResponse(Stream* s, const HttpResponse& resp,
                                  bool no_body) {
    // Header storage must outlive nghttp2_submit_response (it copies the nv
    // array before returning).
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back(":status", std::to_string(resp.statusCode()));
    fields.emplace_back("content-length", std::to_string(resp.body().size()));
    for (const auto& [key, value] : resp.headers()) {
        std::string lname = key;
        std::transform(lname.begin(), lname.end(), lname.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (isConnectionSpecific(lname)) {
            continue;
        }
        fields.emplace_back(std::move(lname), value);
    }
    std::vector<nghttp2_nv> nva;
    nva.reserve(fields.size());
    for (const auto& [n, v] : fields) {
        nva.push_back(makeNv(n, v));
    }

    s->response_body = no_body ? std::string() : resp.body();
    s->body_offset = 0;
    nghttp2_data_provider prd;
    prd.source.ptr = nullptr;
    prd.read_callback = &Http2SessionCallbacks::readBody;
    const bool has_body = !s->response_body.empty();
    nghttp2_submit_response(session_, s->id, nva.data(), nva.size(),
                            has_body ? &prd : nullptr);
}

}  // namespace http
