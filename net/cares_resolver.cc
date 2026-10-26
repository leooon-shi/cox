#include "cares_resolver.h"

#include "event_loop.h"
#include "io_channel.h"
#include "logger.h"

#include <ares.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace net {
namespace {
logging::Logger& log() {
    static logging::Logger logger;
    return logger;
}
}  // namespace

// Impl drives the c-ares channel from the event loop. c-ares tells us which of
// its sockets to watch (read/write); we register each as a Channel and pump
// ares_process_fd when they become ready. A timer polls c-ares' timeout so
// retransmissions fire.
class CaresResolver::Impl : public std::enable_shared_from_this<Impl> {
public:
    explicit Impl(reactor::EventLoop* loop) : loop_(loop) {
        static bool inited = [] {
            ares_library_init(ARES_LIB_INIT_ALL);
            return true;
        }();
        (void)inited;

        struct ares_options options;
        std::memset(&options, 0, sizeof(options));
        int optmask = 0;
        if (ares_init_options(&channel_, &options, optmask) != ARES_SUCCESS) {
            log().error("CaresResolver: ares_init_options failed");
            channel_ = nullptr;
        }
    }

    ~Impl() {
        if (channel_) ares_destroy(channel_);
        // Channels are cleaned up as they're removed.
    }

    struct Query {
        uint16_t port;
        Callback cb;
        std::shared_ptr<Impl> self;  // keep Impl alive until callback
    };

    void resolve(const std::string& host, uint16_t port, Callback cb) {
        if (!channel_) {
            loop_->runInLoop(
                [cb]() { cb(InetAddress(), "resolver not initialized"); });
            return;
        }

        auto* q = new Query{port, std::move(cb), shared_from_this()};
        // Resolve A/AAAA; use ares_getaddrinfo for family-agnostic lookup.
        struct ares_addrinfo_hints hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        ares_getaddrinfo(channel_, host.c_str(), nullptr, &hints,
                         &CaresResolver::Impl::onResult, q);

        updateSockets();

        armTimer();
    }

private:
    static void onResult(void* arg, int status, int /*timeouts*/,
                         struct ares_addrinfo* result) {
        auto* q = static_cast<Query*>(arg);
        std::shared_ptr<Impl> self = q->self;
        uint16_t port = q->port;
        Callback cb = std::move(q->cb);
        delete q;

        if (status != ARES_SUCCESS || result == nullptr) {
            std::string err = ares_strerror(status);
            self->loop_->runInLoop([cb, err]() { cb(InetAddress(), err); });
            if (result) ares_freeaddrinfo(result);
            return;
        }

        InetAddress addr;
        bool ok = false;
        for (auto* node = result->nodes; node != nullptr;
             node = node->ai_next) {
            if (node->ai_family == AF_INET) {
                auto* sin =
                    reinterpret_cast<struct sockaddr_in*>(node->ai_addr);
                sin->sin_port = htons(port);
                addr = InetAddress(*sin);
                ok = true;
                break;
            } else if (node->ai_family == AF_INET6) {
                auto* s6 =
                    reinterpret_cast<struct sockaddr_in6*>(node->ai_addr);
                s6->sin6_port = htons(port);
                addr = InetAddress(*s6);
                ok = true;
                break;
            }
        }

        ares_freeaddrinfo(result);
        if (ok) {
            self->loop_->runInLoop([cb, addr]() { cb(addr, ""); });
        } else {
            self->loop_->runInLoop(
                [cb]() { cb(InetAddress(), "no usable address"); });
        }
    }

    // Register/unregister Channels to match c-ares' desired socket set.
    void updateSockets() {
        ares_socket_t socks[ARES_GETSOCK_MAXNUM];
        int bitmask = ares_getsock(channel_, socks, ARES_GETSOCK_MAXNUM);
        std::map<int, std::unique_ptr<reactor::Channel>> next;
        for (int i = 0; i < ARES_GETSOCK_MAXNUM; ++i) {
            bool readable = ARES_GETSOCK_READABLE(bitmask, i);
            bool writable = ARES_GETSOCK_WRITABLE(bitmask, i);
            if (!readable && !writable) continue;
            int fd = socks[i];
            auto it = channels_.find(fd);
            std::unique_ptr<reactor::Channel> ch;
            if (it != channels_.end()) {
                ch = std::move(it->second);
                channels_.erase(it);
            } else {
                ch = std::make_unique<reactor::Channel>(fd, loop_);
                auto self = shared_from_this();
                ch->setReadCallback([self, fd]() {
                    ares_process_fd(self->channel_, fd, ARES_SOCKET_BAD);
                    self->updateSockets();
                });
                ch->setWriteCallback([self, fd]() {
                    ares_process_fd(self->channel_, ARES_SOCKET_BAD, fd);
                    self->updateSockets();
                });
            }

            // Set interest.
            if (readable)
                ch->enableRead();
            else
                ch->disableRead();
            if (writable)
                ch->enableWrite();
            else
                ch->disableWrite();

            next[fd] = std::move(ch);
        }

        // Any leftover channels are no longer needed: disable + remove.
        for (auto& [fd, ch] : channels_) {
            ch->disableAll();
            ch->remove();
        }
        channels_ = std::move(next);
    }

    void armTimer() {
        struct timeval tv;
        struct timeval* t = ares_timeout(channel_, nullptr, &tv);
        if (!t) return;
        uint64_t ms =
            static_cast<uint64_t>(t->tv_sec) * 1000 + t->tv_usec / 1000;
        if (ms == 0) ms = 1;

        auto self = shared_from_this();
        loop_->runAfter(ms, [self]() {
            ares_process_fd(self->channel_, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
            self->updateSockets();
            self->armTimer();
        });
    }

    reactor::EventLoop* loop_;
    ares_channel channel_{nullptr};
    std::map<int, std::unique_ptr<reactor::Channel>> channels_;
};

CaresResolver::CaresResolver(reactor::EventLoop* loop)
    : impl_(std::make_shared<Impl>(loop)) {}

CaresResolver::~CaresResolver() = default;

void CaresResolver::resolve(const std::string& host, uint16_t port,
                            Callback cb) {
    impl_->resolve(host, port, std::move(cb));
}
}  // namespace net
