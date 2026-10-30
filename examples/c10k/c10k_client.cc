// c10k_client: open N concurrent TCP connections and (optionally) drive
// request/response traffic over them, measuring connect rate and latency.
//
//   c10k_client [--host H] [--port N] [--conns N] [--rate R] [--duration S]
//               [--msg BYTES] [--inflight K] [--src-ips a,b,c]
//               [--split] [--client-nagle]
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
};

[[noreturn]] void usage(const char* p) {
    std::fprintf(stderr,
                 "usage: %s [--host H] [--port N] [--conns N] [--rate R] "
                 "[--duration S] [--msg BYTES] [--inflight K] "
                 "[--src-ips a,b] [--split] [--client-nagle]\n",
                 p);
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
            if (o.msg > 0) {
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
                if (traffic && o.msg > 0) {
                    startRequest(c);
                    flushWrite(c);
                }
            }
            if (c.st != St::kOpen) {
                continue;
            }
            if (e & EPOLLIN) {
                for (;;) {  // edge-triggered: drain until EAGAIN
                    ssize_t r = ::recv(c.fd, rbuf.data(), rbuf.size(), 0);
                    if (r > 0) {
                        size_t got = static_cast<size_t>(r);
                        c.to_read -= std::min(c.to_read, got);
                        if (c.to_read == 0 && traffic && o.msg > 0) {
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
            if (c.st == St::kOpen && (e & EPOLLOUT) && c.to_write > 0) {
                flushWrite(c);
            }

            if (c.st == St::kOpen && (e & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) &&
                !(e & EPOLLIN)) {
                fail(c, ECONNRESET);
            }
        }
    }

    const double secs =
        o.msg > 0 ? std::chrono::duration<double>(Clock::now() - traffic_start)
                        .count()
                  : 0.0;
    std::printf("still open at end: %d / %d\n", connected, o.conns);
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
        if (c.st == St::kOpen) ::close(c.fd);
    }
    ::close(ep);
    return failed > 0 ? 1 : 0;
}
