#include "server_stream.h"

#include "log_macros.h"
#include "logger.h"

#include <utility>

namespace http {
COX_LOG_DEFINE();

ServerStream::ServerStream(HttpRequest request, std::shared_ptr<Sink> sink,
                           size_t high_water)
    : request_(std::move(request)),
      sink_(std::move(sink)),
      high_water_(high_water) {}

ServerStream::~ServerStream() {
    if (state_ != State::kEnded && !aborted_ && sink_) {
        // The handler let go without finishing. kNew: nothing sent yet, the
        // sink answers 500. kStreaming: headers are out, the sink can only
        // terminate the exchange.
        sink_->abandoned();
    }
}

reactor::EventLoop* ServerStream::loop() const {
    return sink_ ? sink_->loop() : nullptr;
}

void ServerStream::respond(const HttpResponse& resp) {
    if (state_ != State::kNew) {
        COX_LOG_WARNING("after a response was started");
        return;
    }
    if (aborted_) {
        state_ = State::kEnded;
        return;
    }
    Headers headers(resp.headers().begin(), resp.headers().end());
    headers["content-length"] = std::to_string(resp.body().size());
    const bool has_body = !resp.body().empty();
    sink_->sendHead(resp.statusCode(), headers, has_body, false);
    if (has_body) {
        sink_->sendData(resp.body());
    }
    sink_->sendEnd();
    state_ = State::kEnded;
    on_writable_ = nullptr;  // release possible self-references (see end())
    on_abort_ = nullptr;
}

void ServerStream::writeHead(int status, Headers headers) {
    if (state_ != State::kNew) {
        COX_LOG_WARNING("called twice");
        return;
    }
    state_ = State::kStreaming;
    if (aborted_) {
        return;
    }
    bool has_length = false;
    for (const auto& [k, v] : headers) {
        std::string lk = k;
        for (auto& c : lk) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        has_length = has_length || lk == "content-length";
    }
    sink_->sendHead(status, headers, /*has_body=*/true,
                    /*chunked_unknown_length=*/!has_length);
}

bool ServerStream::write(std::string_view data) {
    if (state_ != State::kStreaming) {
        COX_LOG_WARNING(state_ == State::kNew ? "before writeHead"
                                              : "after end");
        return false;
    }
    if (aborted_) {
        return false;
    }
    if (data.empty()) {
        return true;
    }
    const size_t buffered = sink_->sendData(data);
    if (buffered >= high_water_) {
        want_writable_ = true;
        return false;
    }
    return true;
}

void ServerStream::end() {
    if (state_ == State::kEnded) {
        return;
    }
    if (state_ == State::kNew) {
        COX_LOG_WARNING("before writeHead");
        return;
    }
    state_ = State::kEnded;
    // Drop the callbacks now that the exchange is over. A handler commonly
    // captures its own ServerStreamPtr in them (to keep writing from
    // onWritable), which is a reference cycle: releasing them here lets the
    // stream be freed.
    on_writable_ = nullptr;
    on_abort_ = nullptr;
    if (!aborted_) {
        sink_->sendEnd();
    }
}

void ServerStream::onAbort(std::function<void()> cb) {
    on_abort_ = std::move(cb);
    if (aborted_ && on_abort_) {
        // Already gone: tell the late subscriber right away.
        auto f = std::move(on_abort_);
        on_abort_ = nullptr;
        f();
    }
}

void ServerStream::notifyAbort() {
    if (aborted_ || state_ == State::kEnded) {
        return;
    }
    aborted_ = true;
    on_writable_ = nullptr;  // never fires after abort; release references
    if (on_abort_) {
        auto f = std::move(on_abort_);
        on_abort_ = nullptr;
        f();
    }
}

void ServerStream::notifyWritable(size_t buffered) {
    if (want_writable_ && buffered < high_water_ && !aborted_ &&
        state_ == State::kStreaming) {
        want_writable_ = false;
        if (on_writable_) {
            on_writable_();
        }
    }
}

}  // namespace http
