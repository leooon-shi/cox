// c10k_client: open N concurrent TCP connections and (optionally) drive
// request/response traffic over them, measuring connect rate and latency.
//
//   c10k_client [--host H] [--port N] [--conns N] [--rate R] [--duration S]
//               [--msg BYTES] [--inflight K] [--src-ips a,b,c]
//               [--split] [--client-nagle] [--attack MODE] [--dribble-ms MS]
//
//   --conns N     total connections to hold open (default 10000).
//   --rate R      new connections per second (0 = as fast as possible).
//                 Rate-limiting connects avoids SYN/accept-queue overflow and
//                 shows the difference between "how many" and "how fast".
//   --duration S  after connecting, keep traffic going for S seconds.
//   --msg BYTES   payload per request (0 = idle connections only).
//   --inflight K  requests in flight per connection (closed loop, default 1).
//   --src-ips     bind to several local IPs round-robin (e.g. 127.0.0.2,...)
//                 to get past the ~28k ephemeral-port limit per destination.
//   --split       send each request as two writes (header + body), the
//                 write-write-read pattern that triggers Nagle x delayed ACK.
//   --client-nagle leave Nagle enabled on the client (default: TCP_NODELAY).
//                 With --split this shows the classic ~40 ms stall.
//   --attack MODE adversarial mode for hardening validation (default none):
//                 slowloris  open N conns, dribble 1 byte every --dribble-ms,
//                            never completing a request. Targets the server's
//                            idle timeout (load-validation scenario S1).
//                 flood      churn connections at max rate: open up to --conns
//                            concurrently, and the instant one completes or is
//                            rejected, close and reopen it, for --duration
//                            seconds. Targets --max-conns / --accept-qps
//                            (scenario S5). Reports attempts, accepts, and
//                            server-side rejections (ECONNRESET/refused).
//                 slow-read  send one request per connection of --msg bytes,
//                            then NEVER read the reply. The server's echo backs
//                            up its output buffer (our receive window fills and
//                            TCP backpressures), so the write timeout force-
//                            closes us. Targets --write-timeout (scenario S2);
//                            use a large --msg so the echo exceeds socket
//                            buffers and actually backs up.
//                 big-body   POST to an HTTP target with a huge declared
//                            Content-Length (1 GiB) and stream the body. A
//                            server with a body-size cap rejects on the
//                            Content-Length (413) and closes BEFORE reading the
//                            body, so almost nothing is uploaded. Targets the
//                            HTTP --max-body limit (scenario S4; point it at
//                            http_target, not the echo server).
//   --dribble-ms MS  slowloris inter-byte gap in ms (default 9000). Keep it
//                 just under the server's --idle so a NON-defended server would
//                 hold the connection open while a defended one reaps it.
//
// Single-threaded, non-blocking, epoll edge-triggered: one client thread can
// hold far more connections than thread-per-connection tools. Report:
// connected/failed counts (with errno histogram), connect p50/p99, request
// QPS and p50/p99/p99.9 latency.
//
// Not part of the Cox library on purpose: a load generator should not share
// code paths with the server it measures.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

// Adversarial mode (hardening validation). kNone is the ordinary load client.
enum class Attack { kNone, kSlowloris, kFlood, kSlowRead, kBigBody };

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 9100;
    int conns = 10000;
    double rate = 0;
    int duration = 5;
    size_t msg = 64;
    int inflight = 1;
    std::vector<std::string> src_ips;
    bool split = false;
    bool client_nagle = false;
    Attack attack = Attack::kNone;
    int dribble_ms = 9000;
};

[[noreturn]] void usage(const char* p) {
    std::fprintf(stderr,
                 "usage: %s [--host H] [--port N] [--conns N] [--rate R] "
                 "[--duration S] [--msg BYTES] [--inflight K] "
                 "[--src-ips a,b] [--split] [--client-nagle] ",
                 "[--attack slowloris|flood|slow-read|big-body] "
                 "[--dribble-ms MS]\n",
                 p);
    std::exit(2);
}

Attack parseAttack(const char* s) {
    std::string a = s;
    if (a == "slowloris") {
        return Attack::kSlowloris;
    }
    if (a == "flood") {
        return Attack::kFlood;
    }
    if (a == "slow-read") {
        return Attack::kSlowRead;
    }
    if (a == "big-body") {
        return Attack::kBigBody;
    }
    std::fprintf(stderr,
                 "unknown --attack mode '%s' (supported: slowloris, flood, "
                 "slow-read, big-body)\n",
                 s);
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage(argv[0]);
            }
            return argv[++i];
        };
        if (a == "--host") {
            o.host = val();
        } else if (a == "--port") {
            o.port = static_cast<uint16_t>(std::atoi(val()));
        } else if (a == "--conns") {
            o.conns = std::atoi(val());
        } else if (a == "--rate") {
            o.rate = std::atof(val());
        } else if (a == "--duration") {
            o.duration = std::atoi(val());
        } else if (a == "--msg") {
            o.msg = std::strtoul(val(), nullptr, 10);
        } else if (a == "--inflight") {
            o.inflight = std::max(1, std::atoi(val()));
        } else if (a == "--src-ips") {
            std::stringstream ss(val());
            for (std::string ip; std::getline(ss, ip, ',');) {
                o.src_ips.push_back(ip);
            }
        } else if (a == "--split") {
            o.split = true;
        } else if (a == "--client-nagle") {
            o.client_nagle = true;
        } else if (a == "--attack") {
            o.attack = parseAttack(val());
        } else if (a == "--dribble-ms") {
            o.dribble_ms = std::max(1, std::atoi(val()));
        } else {
            usage(argv[0]);
        }
    }
    return o;
}

enum class St { kConnecting, kOpen, kDead };

struct Conn {
    int fd = -1;
    St st = St::kConnecting;
    Clock::time_point connect_start;
    size_t to_write = 0;  // bytes of the current batch still to send
    size_t to_read = 0;   // bytes still expected back
    size_t split_at = 0;  // --split: remaining-bytes mark of the 2nd write
    Clock::time_point req_start;
    Clock::time_point next_dribble;  // slowloris: when to send the next byte
};

double pct(std::vector<double>& v, double p) {
    if (v.empty()) {
        return 0;
    }
    size_t i = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(i), v.end());
    return v[i];
}

void raiseFdLimit(int want) {
    rlimit rl{};
    ::getrlimit(RLIMIT_NOFILE, &rl);
    rlim_t need = static_cast<rlim_t>(want) + 64;
    if (rl.rlim_cur < need) {
        rl.rlim_cur = std::min(need, rl.rlim_max);
        ::setrlimit(RLIMIT_NOFILE, &rl);
        ::getrlimit(RLIMIT_NOFILE, &rl);
    }
    if (rl.rlim_cur < need) {
        std::fprintf(
            stderr,
            "warning: RLIMIT_NOFILE=%llu < %llu needed; raise the hard "
            "limit (ulimit -Hn / limits.conf)\n",
            static_cast<unsigned long long>(rl.rlim_cur),
            static_cast<unsigned long long>(need));
    }
}
}  // namespace

// Flood attack (scenario S5): churn connections at maximum rate to stress the
// server's accept path and its --max-conns / --accept-qps defenses. Up to
// `o.conns` connects are kept in flight; the instant a slot resolves (accepted,
// refused, or reset) it is recycled into a fresh connect. Runs for o.duration
// seconds, then reports attempts / accepted / failure-by-errno. Self-contained
// (does not touch the hold-forever state machine in main's normal/slowloris
// path), but reuses the same non-blocking connect + epoll mechanics.
int runFlood(const Options& o, const sockaddr_in& dst) {
    const int ep = ::epoll_create1(EPOLL_CLOEXEC);
    std::vector<Conn> slots(static_cast<size_t>(o.conns));
    std::map<int, int> errors;
    std::vector<char> rbuf(4096);

    uint64_t attempts = 0, accepted = 0, failed = 0;
    const auto t0 = Clock::now();
    const auto deadline = t0 + std::chrono::seconds(o.duration);

    // Start (or restart) a connect on slot `idx`. Returns false if the socket
    // could not even be created (counted as a failure by the caller).
    auto startConnect = [&](uint32_t idx) -> bool {
        Conn& c = slots[idx];
        ++attempts;
        c.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (c.fd < 0) {
            ++errors[errno];
            ++failed;
            c.st = St::kDead;
            return false;
        }
        c.st = St::kConnecting;
        c.connect_start = Clock::now();
        int r = ::connect(c.fd, reinterpret_cast<const sockaddr*>(&dst),
                          sizeof dst);
        if (r < 0 && errno != EINPROGRESS) {
            ++errors[errno];
            ++failed;
            ::close(c.fd);
            c.st = St::kDead;
            return false;
        }
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
        ev.data.u32 = idx;
        ::epoll_ctl(ep, EPOLL_CTL_ADD, c.fd, &ev);
        return true;
    };
    auto recycle = [&](uint32_t idx) {
        Conn& c = slots[idx];
        if (c.fd >= 0) {
            ::epoll_ctl(ep, EPOLL_CTL_DEL, c.fd, nullptr);
            ::close(c.fd);
            c.fd = -1;
        }
        // Only keep churning while within the attack window.
        if (Clock::now() < deadline) {
            startConnect(idx);
        } else {
            c.st = St::kDead;
        }
    };

    // Prime all slots.
    for (uint32_t i = 0; i < static_cast<uint32_t>(o.conns); ++i) {
        startConnect(i);
    }

    std::vector<epoll_event> evs(4096);
    int inflight = o.conns;  // optimistic; recycle() keeps it topped up
    (void)inflight;
    while (Clock::now() < deadline) {
        int n = ::epoll_wait(ep, evs.data(), static_cast<int>(evs.size()), 50);
        for (int i = 0; i < n; ++i) {
            uint32_t idx = evs[static_cast<size_t>(i)].data.u32;
            uint32_t e = evs[static_cast<size_t>(i)].events;
            Conn& c = slots[idx];
            if (c.st == St::kDead) {
                continue;
            }
            if (c.st == St::kConnecting &&
                (e & (EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP))) {
                int err = 0;
                socklen_t len = sizeof err;
                ::getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err != 0 || (e & (EPOLLERR | EPOLLHUP))) {
                    // Connect refused/reset: a server-side shed (cap or rate
                    // limit closes the fd) looks like ECONNRESET/ECONNREFUSED.
                    ++errors[err != 0 ? err : ECONNRESET];
                    ++failed;
                    recycle(idx);
                    continue;
                }
                ++accepted;
                c.st = St::kOpen;
                // Accepted. For a pure accept-path flood we do not send data;
                // immediately recycle to keep connect pressure maximal.
                recycle(idx);
                continue;
            }
            if (c.st == St::kOpen) {
                // Drain anything and recycle (server may have closed us).
                (void)::recv(c.fd, rbuf.data(), rbuf.size(), 0);
                recycle(idx);
            }
        }
    }

    // Final close of any still-open fds.
    for (auto& c : slots) {
        if (c.fd >= 0) {
            ::close(c.fd);
        }
    }
    ::close(ep);

    const double secs =
        std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf(
        "flood: %llu attempts, %llu accepted, %llu failed in %.1fs "
        "(%.0f attempts/s)\n",
        static_cast<unsigned long long>(attempts),
        static_cast<unsigned long long>(accepted),
        static_cast<unsigned long long>(failed), secs,
        secs > 0 ? static_cast<double>(attempts) / secs : 0.0);
    if (!errors.empty()) {
        std::printf("flood failures by errno:");
        for (auto& [err, cnt] : errors) {
            std::printf(" %s=%d", std::strerror(err), cnt);
        }
        std::printf("\n");
    }
    std::printf(
        "interpretation: against --max-conns/--accept-qps, 'accepted' is "
        "bounded by the cap+rate and 'failed' (reset/refused) counts the "
        "shed connections. Authoritative containment: server-side "
        "cox_connections_live<=cap and cox_connections_rejected_total "
        "advancing (via /metrics).\n");
    return 0;
}

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    raiseFdLimit(o.conns);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(o.port);
    if (::inet_pton(AF_INET, o.host.c_str(), &dst.sin_addr) != 1) {
        std::fprintf(stderr, "bad host %s (IPv4 literal expected)\n",
                     o.host.c_str());
        return 2;
    }

    // Flood is a distinct traffic shape (connection churn, not a held pool), so
    // it runs its own self-contained loop rather than the hold-forever path.
    if (o.attack == Attack::kFlood) {
        return runFlood(o, dst);
    }

    int ep = ::epoll_create1(EPOLL_CLOEXEC);
    std::vector<Conn> conns(static_cast<size_t>(o.conns));
    std::map<int, int> errors;  // errno -> count
    std::vector<double> connect_us, req_us;
    connect_us.reserve(conns.size());
    req_us.reserve(1 << 20);
    const std::string payload(
        std::max<size_t>(o.msg, 1) * static_cast<size_t>(o.inflight), 'x');
    std::vector<char> rbuf(64 * 1024);

    int opened = 0, connected = 0, failed = 0;
    uint64_t requests = 0;
    const auto t0 = Clock::now();
    auto traffic_start = Clock::time_point{};
    bool traffic = false;

    auto fail = [&](Conn& c, int err) {
        if (c.st != St::kDead) {
            ++errors[err];
            ++failed;
            if (c.st == St::kOpen) {
                --connected;
            }
            c.st = St::kDead;
            ::close(c.fd);
        }
    };
    auto startRequest = [&](Conn& c) {
        c.to_write = o.msg * static_cast<size_t>(o.inflight);
        c.split_at = o.split ? c.to_write / 2 : 0;
        c.to_read = c.to_write;
        c.req_start = Clock::now();
    };
    auto flushWrite = [&](Conn& c) {
        while (c.to_write > 0) {
            // With --split, stop the first write at the midpoint so the
            // request leaves as two separate send() calls.
            size_t chunk = c.to_write;
            if (c.split_at > 0 && c.to_write > c.split_at) {
                chunk = c.to_write - c.split_at;
            }
            ssize_t n =
                ::send(c.fd, payload.data() + (payload.size() - c.to_write),
                       chunk, MSG_NOSIGNAL);
            if (n > 0) {
                c.to_write -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            fail(c, n < 0 ? errno : EPIPE);
            return;
        }
    };

    // Slowloris: an HTTP request that is never completed. We send the request
    // line, then dribble header bytes one at a time, and NEVER send the final
    // blank line, so the server's HTTP/request state stays "mid-request" and
    // the connection is only reclaimable by an idle/read timeout. The byte
    // stream cycles a filler header forever; `c.to_write` is reused as the
    // index of the next byte to emit (it is otherwise unused in this mode).
    static const std::string kSlowReq = "GET / HTTP/1.1\r\nX-Pad: ";
    static const std::string kSlowFill =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ";
    auto slowlorisByteAt = [&](size_t i) -> char {
        if (i < kSlowReq.size()) {
            return kSlowReq[i];
        }
        // After the prefix, emit filler bytes forever (never the terminating
        // CRLFCRLF), so the request never completes.
        return kSlowFill[(i - kSlowReq.size()) % kSlowFill.size()];
    };
    // Send one slowloris byte; returns false if the connection was torn down.
    auto dribbleOne = [&](Conn& c) {
        char ch = slowlorisByteAt(c.to_write);
        ssize_t n = ::send(c.fd, &ch, 1, MSG_NOSIGNAL);
        if (n == 1) {
            ++c.to_write;  // index of the next byte to send
            return;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // socket buffer full (unlikely for 1 byte); retry next gap
        }
        fail(c, n < 0 ? errno : EPIPE);
    };

    // big-body (S4): a POST with a huge declared Content-Length. A server with
    // a body-size cap rejects it UP FRONT on the Content-Length (413) and
    // closes before reading the body, so the client should send only the header
    // plus a little body before the connection drops. `split_at` doubles as a
    // "header already sent" flag (0 = not yet, 1 = sent); `to_write` holds the
    // body bytes still to stream (capped at o.msg so the loop terminates if the
    // server does NOT reject -- an undefended server would then see o.msg
    // bytes).
    const std::string kBigHeader =
        "POST /sink HTTP/1.1\r\nHost: target\r\nContent-Type: "
        "application/octet-stream\r\nContent-Length: 1073741824\r\n\r\n";
    auto bigBodySend = [&](Conn& c) {
        if (c.split_at == 0) {  // send the header once
            size_t off = 0;
            while (off < kBigHeader.size()) {
                ssize_t n = ::send(c.fd, kBigHeader.data() + off,
                                   kBigHeader.size() - off, MSG_NOSIGNAL);
                if (n > 0) {
                    off += static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return;  // retry on next EPOLLOUT
                }
                fail(c, n < 0 ? errno : EPIPE);
                return;
            }
            c.split_at = 1;  // header fully sent
        }
        // Stream body bytes (bounded by o.msg so this terminates even if the
        // server never rejects). A defended server closes us well before this.
        while (c.to_write > 0) {
            size_t chunk = std::min(c.to_write, payload.size());
            ssize_t n = ::send(c.fd, payload.data(), chunk, MSG_NOSIGNAL);
            if (n > 0) {
                c.to_write -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;  // retry on next EPOLLOUT
            }
            fail(c, n < 0 ? errno : EPIPE);
            return;
        }
    };

    std::vector<epoll_event> evs(4096);
    for (;;) {
        // 1. Open new connections, paced by --rate.
        auto now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - t0).count();
        int allowed =
            o.rate > 0
                ? std::min(o.conns, static_cast<int>(elapsed * o.rate) + 1)
                : o.conns;
        for (int burst = 0; opened < allowed && burst < 512;
             ++burst, ++opened) {
            Conn& c = conns[static_cast<size_t>(opened)];
            c.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                            0);
            if (c.fd < 0) {
                ++errors[errno];
                ++failed;
                c.st = St::kDead;
                continue;
            }
            if (!o.src_ips.empty()) {
                sockaddr_in src{};
                src.sin_family = AF_INET;
                ::inet_pton(
                    AF_INET,
                    o.src_ips[static_cast<size_t>(opened) % o.src_ips.size()]
                        .c_str(),
                    &src.sin_addr);
                int one =
                    1;  // let the kernel pick a port per 4-tuple, not per IP
                ::setsockopt(c.fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one,
                             sizeof one);
                ::bind(c.fd, reinterpret_cast<sockaddr*>(&src), sizeof src);
            }
            c.connect_start = Clock::now();
            int r =
                ::connect(c.fd, reinterpret_cast<sockaddr*>(&dst), sizeof dst);
            if (r < 0 && errno != EINPROGRESS) {
                fail(c, errno);
                continue;
            }
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
            ev.data.u32 = static_cast<uint32_t>(opened);
            ::epoll_ctl(ep, EPOLL_CTL_ADD, c.fd, &ev);
        }

        // 2. Once everything is connected (or failed), start the traffic phase.
        if (!traffic && opened == o.conns && connected + failed == o.conns) {
            traffic = true;
            traffic_start = Clock::now();
            double ct =
                std::chrono::duration<double>(traffic_start - t0).count();
            std::printf(
                "connect phase: %d connected, %d failed in %.2fs (%.0f "
                "conn/s)\n",
                connected, failed, ct, ct > 0 ? connected / ct : 0.0);
            std::fflush(stdout);
            if ((o.attack == Attack::kNone || o.attack == Attack::kSlowRead) &&
                o.msg > 0) {
                // Normal: closed-loop request/response. slow-read: send ONE
                // request per connection and then never read the reply, so the
                // server's echo backs up its output buffer (the write-timeout
                // target). Both just prime the first request here.
                for (auto& c : conns) {
                    if (c.st == St::kOpen) {
                        startRequest(c);
                        flushWrite(c);
                    }
                }
            }
        }
        // Stop after --duration seconds of the traffic phase (or of holding
        // idle connections when --msg 0).
        if (traffic &&
            std::chrono::duration<double>(Clock::now() - traffic_start)
                    .count() >= o.duration) {
            break;
        }

        // 3. Handle readiness.
        int n = ::epoll_wait(ep, evs.data(), static_cast<int>(evs.size()), 10);
        for (int i = 0; i < n; ++i) {
            Conn& c = conns[evs[static_cast<size_t>(i)].data.u32];
            uint32_t e = evs[static_cast<size_t>(i)].events;
            if (c.st == St::kDead) {
                continue;
            }
            if (c.st == St::kConnecting &&
                (e & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
                int err = 0;
                socklen_t len = sizeof err;
                ::getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err != 0) {
                    fail(c, err);
                    continue;
                }
                c.st = St::kOpen;
                ++connected;
                int one = 1;
                if (!o.client_nagle) {
                    ::setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one,
                                 sizeof one);
                }
                connect_us.push_back(std::chrono::duration<double, std::micro>(
                                         Clock::now() - c.connect_start)
                                         .count());
                if (o.attack == Attack::kSlowloris) {
                    // Begin dribbling immediately; to_write is the byte index
                    // (already 0). Send the first byte now, schedule the rest.
                    dribbleOne(c);
                    c.next_dribble =
                        Clock::now() + std::chrono::milliseconds(o.dribble_ms);
                } else if (o.attack == Attack::kBigBody) {
                    // Send the POST header + stream body; a defended server
                    // rejects on Content-Length (413) and closes early.
                    c.split_at = 0;  // header not yet sent
                    c.to_write = std::max<size_t>(o.msg, 1) *
                                 static_cast<size_t>(o.inflight);
                    bigBodySend(c);
                } else if (traffic && o.msg > 0) {
                    startRequest(c);
                    flushWrite(c);
                }
            }
            if (c.st != St::kOpen) {
                continue;
            }
            // slow-read (S2): deliberately do NOT read the response. Leaving
            // the echoed bytes unread fills our receive window; TCP
            // backpressures the server, whose output buffer then backs up until
            // the write timeout force-closes us. Reading would relieve that
            // pressure and defeat the test. We still fall through to the
            // RDHUP/HUP/ERR check below to observe the server-initiated close
            // (the defense firing).
            if ((e & EPOLLIN) && o.attack != Attack::kSlowRead) {
                for (;;) {  // edge-triggered: drain until EAGAIN
                    ssize_t r = ::recv(c.fd, rbuf.data(), rbuf.size(), 0);
                    if (r > 0) {
                        // Slowloris: the echo server reflects our dribbled
                        // bytes; drain and ignore them (no request accounting).
                        if (o.attack == Attack::kSlowloris) {
                            continue;
                        }
                        // big-body: the reply is the 413 (or an undefended
                        // 200); drain it, no closed-loop re-request.
                        if (o.attack == Attack::kBigBody) {
                            continue;
                        }
                        size_t got = static_cast<size_t>(r);
                        c.to_read -= std::min(c.to_read, got);
                        if (c.to_read == 0 && traffic &&
                            o.attack == Attack::kNone && o.msg > 0) {
                            req_us.push_back(
                                std::chrono::duration<double, std::micro>(
                                    Clock::now() - c.req_start)
                                    .count());
                            requests += static_cast<uint64_t>(o.inflight);
                            startRequest(c);
                            flushWrite(c);
                        }
                        continue;
                    }
                    if (r == 0) {
                        fail(c, ECONNRESET);
                        break;
                    }  // server closed
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    fail(c, errno);
                    break;
                }
            }
            // Continue flushing a backed-up request. slow-read must finish
            // SENDING its (large) request so the server has the full payload to
            // echo back -- it just never READs the reply. Slowloris reuses
            // to_write as a dribble index (handled separately) and flood has no
            // request, so EPOLLOUT flushing applies only to kNone and
            // slow-read.
            if (c.st == St::kOpen &&
                (o.attack == Attack::kNone || o.attack == Attack::kSlowRead) &&
                (e & EPOLLOUT) && c.to_write > 0) {
                flushWrite(c);
            }
            // big-body continues streaming its POST (header first, then body)
            // on writability until the server rejects+closes it.
            if (c.st == St::kOpen && o.attack == Attack::kBigBody &&
                (e & EPOLLOUT)) {
                bigBodySend(c);
            }

            // Detect a server-initiated close. Normally we require !EPOLLIN so
            // pending data is drained first (the final response is read before
            // we act on the hangup). slow-read never reads, so for it we must
            // honour the hangup even with EPOLLIN set, or we would never see
            // the write-timeout close.
            if (c.st == St::kOpen && (e & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) &&
                (o.attack == Attack::kSlowRead || !(e & EPOLLIN))) {
                fail(c, ECONNRESET);
            }
        }

        // 4. Slowloris dribble sweep: emit one byte on every open connection
        // whose inter-byte gap has elapsed. O(conns) per iteration, but the
        // epoll_wait timeout (10 ms) bounds how often it runs, and a byte is
        // only sent once per --dribble-ms, so the real work is tiny.
        if (o.attack == Attack::kSlowloris) {
            auto tick = Clock::now();
            for (auto& c : conns) {
                if (c.st == St::kOpen && tick >= c.next_dribble) {
                    dribbleOne(c);
                    c.next_dribble =
                        tick + std::chrono::milliseconds(o.dribble_ms);
                }
            }
        }
    }

    const bool request_mode = o.attack == Attack::kNone && o.msg > 0;
    const double secs =
        request_mode
            ? std::chrono::duration<double>(Clock::now() - traffic_start)
                  .count()
            : 0.0;
    std::printf("still open at end: %d / %d\n", connected, o.conns);
    if (o.attack == Attack::kSlowloris) {
        // Interpretation: against a DEFENDED server, most/all slowloris
        // connections are idle-reaped, so "still open" trends toward 0 and the
        // errno histogram shows the server-initiated closes. Against an
        // UNDEFENDED server they stay open (still open ~= conns) -- the failure
        // the idle timeout exists to prevent. The authoritative containment
        // signal is server-side (cox_connections_live plateau via /metrics).
        std::printf(
            "slowloris: held %d/%d open at end for %ds (dribble=%dms). "
            "still-open near 0 => server idle-reaped the conns; "
            "still-open near conns => dribble beat the idle timeout (needs a "
            "connection cap to bound, see S5).\n",
            connected, o.conns, o.duration, o.dribble_ms);
    }
    if (o.attack == Attack::kSlowRead) {
        // Against a DEFENDED server the write timeout force-closes us once the
        // echo has backed up past the deadline, so "still open" trends to 0 and
        // the errno histogram shows the server closes. Authoritative signal is
        // server-side: cox_connections_closed_total{reason="write_timeout"}
        // advancing and VmRSS bounded (via /metrics + the sampler).
        std::printf(
            "slow-read: held %d/%d open at end (msg=%zuB unread). still-open "
            "near 0 => server write-timeout closed the slow readers.\n",
            connected, o.conns, o.msg);
    }
    if (o.attack == Attack::kBigBody) {
        // Against a DEFENDED HTTP target the body cap rejects the declared
        // Content-Length with 413 and closes before reading the body, so the
        // connections drop almost immediately (still-open ~0) and the server
        // buffers ~nothing. Authoritative signal is server-side:
        // cox_http_requests_total{status_class="4xx"} advancing + flat VmRSS.
        std::printf(
            "big-body: %d/%d still open at end. still-open near 0 => target "
            "rejected the oversized body (413) and closed; check "
            "cox_http_requests_total{status_class=\"4xx\"} and flat RSS.\n",
            connected, o.conns);
    }
    if (!errors.empty()) {
        std::printf("failures by errno:");
        for (auto& [err, cnt] : errors) {
            std::printf(" %s=%d", std::strerror(err), cnt);
        }
        std::printf("\n");
    }
    std::printf("connect latency: p50=%.0fus p99=%.0fus max=%.0fus\n",
                pct(connect_us, 0.50), pct(connect_us, 0.99),
                pct(connect_us, 1.0));
    if (o.msg > 0 && secs > 0) {
        std::printf(
            "requests: %llu in %.1fs = %.0f req/s (msg=%zuB, inflight=%d)\n",
            static_cast<unsigned long long>(requests), secs,
            static_cast<double>(requests) / secs, o.msg, o.inflight);
        std::printf(
            "request latency: p50=%.0fus p99=%.0fus p99.9=%.0fus max=%.0fus\n",
            pct(req_us, 0.50), pct(req_us, 0.99), pct(req_us, 0.999),
            pct(req_us, 1.0));
    }
    for (auto& c : conns) {
        if (c.st == St::kOpen) {
            ::close(c.fd);
        }
    }
    ::close(ep);
    return failed > 0 ? 1 : 0;
}
