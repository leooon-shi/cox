// probe_client: a single well-behaved client that issues ONE request per
// second against the data port, recording success and round-trip latency. It
// is the liveness half of the load-validation thesis (docs/insights/features/
// load-validation-design.md): while an adversary (c10k_client --attack ...)
// hammers the server, this probe answers "is a legitimate client still served,
// with bounded latency?" A defense that protects a resource by freezing all
// traffic fails this measurement.
//
//   probe_client [--host H] [--port N] [--interval-ms MS] [--count N]
//                [--timeout-ms MS] [--msg BYTES] [--quiet] [--no-read]
//
//   --host H         data-port host (IPv4 literal; default 127.0.0.1).
//   --port N         data port under attack (default 9100).
//   --interval-ms    time between probes (default 1000 = 1/s, per the design).
//   --count N        number of probes to issue (0 = run until killed).
//   --timeout-ms     per-probe deadline covering connect+send+recv (default
//                    1000). A probe that exceeds it is a FAILURE (timeout) --
//                    this is how a server that stops serving shows up.
//   --msg BYTES      echo payload size (default 16); the server echoes it back.
//   --quiet          suppress the per-probe line; print only the summary.
//   --no-read        success = connect+send completes in time; do NOT wait for
//                    a reply. For sink-mode targets (scenario S3) that never
//                    echo, where liveness means "the accept/read path stays
//                    responsive," not "a reply comes back."
//   --http           send a minimal "GET / HTTP/1.1" request instead of raw
//                    filler, and count ANY HTTP response as success. Use this
//                    to probe an HTTP target (scenario S4), which speaks HTTP,
//                    not raw echo.
//
// A FRESH connection per probe on purpose: it exercises the accept path too, so
// an attack that starves accept() (flood, fd exhaustion) surfaces as connect
// failures here, not just as degraded latency on a pre-opened connection.
//
// Standalone (libc/POSIX only), like c10k_client: a measurement tool must not
// share code paths with the server it measures.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 9100;
    int interval_ms = 1000;
    int count = 0;
    int timeout_ms = 1000;
    size_t msg = 16;
    bool quiet = false;
    bool no_read = false;  // success = connect+send only (no echo expected)
    bool http = false;     // send a minimal HTTP GET; success = any HTTP reply
};

[[noreturn]] void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--host H] [--port N] [--interval-ms MS] "
                 "[--count N] [--timeout-ms MS] [--msg BYTES] [--quiet] "
                 "[--no-read] [--http]\n",
                 prog);
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
        } else if (a == "--interval-ms") {
            o.interval_ms = std::max(1, std::atoi(val()));
        } else if (a == "--count") {
            o.count = std::atoi(val());
        } else if (a == "--timeout-ms") {
            o.timeout_ms = std::max(1, std::atoi(val()));
        } else if (a == "--msg") {
            o.msg = std::max<size_t>(1, std::strtoul(val(), nullptr, 10));
        } else if (a == "--quiet") {
            o.quiet = true;
        } else if (a == "--no-read") {
            o.no_read = true;
        } else if (a == "--http") {
            o.http = true;
        } else {
            usage(argv[0]);
        }
    }
    return o;
}

double pct(std::vector<double>& v, double p) {
    if (v.empty()) {
        return 0;
    }
    size_t i = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(i), v.end());
    return v[i];
}

// Milliseconds remaining until `deadline`, clamped to >= 0, for poll().
int msUntil(Clock::time_point deadline) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - Clock::now())
                  .count();
    if (ms < 0) {
        return 0;
    }
    return static_cast<int>(ms);
}

// Wait for `fd` to become ready for `events` (POLLIN/POLLOUT) before deadline.
// Returns the revents, or 0 on timeout, or -1 on poll error.
short waitFd(int fd, short events, Clock::time_point deadline) {
    pollfd pfd{fd, events, 0};
    int r = ::poll(&pfd, 1, msUntil(deadline));
    if (r > 0) {
        return pfd.revents;
    }
    return static_cast<short>(r);  // 0 timeout, -1 error
}

enum class Result { kOk, kConnectFail, kTimeout, kIoFail };

// One probe: fresh non-blocking connect + send(msg) + recv(msg) within the
// deadline. Fills `latency_us` on success. Keeps its own fd; never leaks it.
Result probeOnce(const Options& o, const sockaddr_in& dst,
                 const std::string& payload, std::vector<char>& rbuf,
                 double* latency_us) {
    auto start = Clock::now();
    auto deadline = start + std::chrono::milliseconds(o.timeout_ms);

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return Result::kConnectFail;
    }
    struct FdGuard {
        int fd;
        ~FdGuard() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } guard{fd};

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    int r = ::connect(fd, reinterpret_cast<const sockaddr*>(&dst), sizeof dst);
    if (r < 0 && errno != EINPROGRESS) {
        return Result::kConnectFail;
    }
    if (r < 0) {
        short re = waitFd(fd, POLLOUT, deadline);
        if (re <= 0) {
            return Result::kTimeout;
        }
        int err = 0;
        socklen_t len = sizeof err;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            return Result::kConnectFail;
        }
    }

    // Send the whole payload (small; one write almost always suffices, but
    // loop to be correct under a tiny send buffer).
    size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t n = ::send(fd, payload.data() + sent, payload.size() - sent,
                           MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (waitFd(fd, POLLOUT, deadline) <= 0) {
                return Result::kTimeout;
            }
            continue;
        }
        return Result::kIoFail;
    }

    // --no-read: success is connect+send completing in time. Used for sink-mode
    // targets (S3) that never echo, where liveness means the accept/read path
    // stays responsive, not that a reply comes back.
    if (o.no_read) {
        *latency_us =
            std::chrono::duration<double, std::micro>(Clock::now() - start)
                .count();
        return Result::kOk;
    }

    // --http: success is receiving ANY response bytes (an HTTP status line).
    // We do not match the body; getting a reply at all proves the target
    // accepted, parsed, and answered a well-formed request while under attack.
    if (o.http) {
        if (waitFd(fd, POLLIN, deadline) <= 0) {
            return Result::kTimeout;
        }
        ssize_t n = ::recv(fd, rbuf.data(), rbuf.size(), 0);
        if (n > 0) {
            *latency_us =
                std::chrono::duration<double, std::micro>(Clock::now() - start)
                    .count();
            return Result::kOk;
        }
        return Result::kIoFail;  // closed/err before any reply
    }

    // Read the echo back (expect msg bytes).
    size_t got = 0;
    while (got < payload.size()) {
        if (waitFd(fd, POLLIN, deadline) <= 0) {
            return Result::kTimeout;
        }
        ssize_t n = ::recv(fd, rbuf.data(), rbuf.size(), 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            return Result::kIoFail;  // server closed before echoing
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        return Result::kIoFail;
    }

    *latency_us =
        std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    return Result::kOk;
}

const char* resultName(Result r) {
    switch (r) {
        case Result::kOk:
            return "ok";
        case Result::kConnectFail:
            return "connect_fail";
        case Result::kTimeout:
            return "timeout";
        case Result::kIoFail:
            return "io_fail";
    }
    return "?";
}
}  // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(o.port);
    if (::inet_pton(AF_INET, o.host.c_str(), &dst.sin_addr) != 1) {
        std::fprintf(stderr, "bad host %s (IPv4 literal expected)\n",
                     o.host.c_str());
        return 2;
    }

    // Payload: raw filler for the echo probe, or a minimal valid HTTP GET for
    // an HTTP target (--http). Connection: close so the server ends the
    // response cleanly and we measure one full round trip per probe.
    const std::string payload =
        o.http ? std::string("GET / HTTP/1.1\r\nHost: " + o.host +
                             "\r\nConnection: close\r\n\r\n")
               : std::string(o.msg, 'p');
    std::vector<char> rbuf(std::max<size_t>(o.msg, 64));

    std::vector<double> ok_us;
    uint64_t total = 0, ok = 0, connect_fail = 0, timeout = 0, io_fail = 0;

    std::printf("probe: 1 req/%dms to %s:%u (msg=%zuB, timeout=%dms)\n",
                o.interval_ms, o.host.c_str(), o.port, o.msg, o.timeout_ms);
    std::fflush(stdout);

    auto next = Clock::now();
    for (int i = 0; o.count == 0 || i < o.count; ++i) {
        next += std::chrono::milliseconds(o.interval_ms);

        double lat = 0;
        Result r = probeOnce(o, dst, payload, rbuf, &lat);
        ++total;
        switch (r) {
            case Result::kOk:
                ++ok;
                ok_us.push_back(lat);
                break;
            case Result::kConnectFail:
                ++connect_fail;
                break;
            case Result::kTimeout:
                ++timeout;
                break;
            case Result::kIoFail:
                ++io_fail;
                break;
        }
        if (!o.quiet) {
            if (r == Result::kOk) {
                std::printf("probe %4d: ok %.0fus\n", i, lat);
            } else {
                std::printf("probe %4d: FAIL (%s)\n", i, resultName(r));
            }
            std::fflush(stdout);
        }

        // Sleep until the next cadence tick (skip if a slow probe overran it).
        auto now = Clock::now();
        if (now < next) {
            std::this_thread::sleep_until(next);
        }
    }

    const double rate =
        total > 0 ? 100.0 * static_cast<double>(ok) / static_cast<double>(total)
                  : 0.0;
    std::printf(
        "probe summary: %llu/%llu ok (%.1f%%) "
        "fail{connect=%llu timeout=%llu io=%llu}\n",
        static_cast<unsigned long long>(ok),
        static_cast<unsigned long long>(total), rate,
        static_cast<unsigned long long>(connect_fail),
        static_cast<unsigned long long>(timeout),
        static_cast<unsigned long long>(io_fail));
    std::printf("probe latency (ok only): p50=%.0fus p99=%.0fus max=%.0fus\n",
                pct(ok_us, 0.50), pct(ok_us, 0.99), pct(ok_us, 1.0));
    // Nonzero exit if any probe failed: a CI liveness assertion can key on it.
    return (connect_fail + timeout + io_fail) > 0 ? 1 : 0;
}
