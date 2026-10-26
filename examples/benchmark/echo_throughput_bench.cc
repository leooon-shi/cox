// Echo throughput & latency benchmark for the Cox TCP stack.
//
// Spins up a real TcpServer echo endpoint (optionally multi-threaded), then
// drives it with N concurrent client connections each issuing a request and
// waiting for the echo (closed-loop). Reports QPS, throughput, and latency
// percentiles (p50/p90/p99/max).
//
// Usage:
//   echo_throughput_bench [connections] [duration_s] [msg_size] [server_threads]
// Defaults: 100 connections, 5s, 128-byte messages, 1 server thread.

#include "buffer.h"
#include "event_loop.h"
#include "inet_address.h"
#include "socket.h"
#include "tcp_connection.h"
#include "tcp_server.h"

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;
using namespace net;

namespace
{
using Clock = steady_clock;

// One closed-loop client connection: connect, then repeatedly send a message
// and block for the echo, recording per-request latency, until stop is set.
struct ClientResult
{
    uint64_t requests = 0;
    uint64_t bytes = 0;
    std::vector<double> latencies_us;       // Sampled
};

void runClient(uint16_t port, size_t msg_size, std::atomic<bool>* stop,
                    ClientResult* out)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 )
    {
        ::close(fd);
        return;
    }

    std::string msg(msg_size, 'x');
    std::vector<char> buf(msg_size);
    out->latencies_us.reserve(1 << 16);

    while (!stop->load(std::memory_order_relaxed))
    {
        auto t0 = Clock::now();
        if (::write(fd, msg.data(), msg.size()) != static_cast<ssize_t>(msg.size()))
        {
            break;
        }
        // Read exactly msg_size bytes back (echo).
        size_t got = 0;
        while (got < msg_size)
        {
            ssize_t n = ::read(fd, buf.data() + got, msg_size - got);
            if (n <= 0) { got = 0; break; }
            got += static_cast<size_t>(n);
        }
        if (got != msg_size) break;
        auto t1 = Clock::now();

        out->requests++;
        out->bytes += msg_size;
        // Sample latency (keep memory bounded).
        if ((out->requests & 0x3) == 0) {  // 1 in 4
            out->latencies_us.push_back(
                duration_cast<nanoseconds>(t1 - t0).count() / 1000.0);
        }
    }
    ::close(fd);
}

double percentile(std::vector<double>& v, double p)
{
    if (v.empty()) return 0;
    size_t idx = static_cast<size_t>(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}
}   // namespace

int main(int argc, char* argv[])
{
    int connections = (argc > 1) ? std::atoi(argv[1]) : 100;
    int duration_s = (argc > 2) ? std::atoi(argv[2]) : 5;
    size_t msg_size = (argc > 3) ? static_cast<size_t>(std::atoi(argv[3])) : 128;
    int server_threads = (argc > 4) ? std::atoi(argv[4]) : 1;
    const uint16_t port = 54321;

    // ---- Server ----
    std::atomic<bool> srv_started{false};
    reactor::EventLoop* srv_loop = nullptr;
    std::thread server_thread([&] {
        reactor::EventLoop loop;
        srv_loop = &loop;
        InetAddress addr("127.0.0.1", port);
        TcpServer server(&loop, addr, "bench-echo");
        if (server_threads > 1) server.setThreadNum(server_threads);
        server.setMessageCallback(
            [](const TcpConnectionPtr& conn, buffer::Buffer* buf) {
            conn->send(buf->retrieveAllAsString());
        });
        server.start();
        srv_started = true;
        loop.loop();
    });

    while (!srv_started.load()) std::this_thread::sleep_for(milliseconds(1));
    std::this_thread::sleep_for(milliseconds(50));

    // ---- Clients ----
    std::atomic<bool> stop{false};
    std::vector<ClientResult> results(connections);
    std::vector<std::thread> clients;
    auto bench_start = Clock::now();
    for (int i = 0; i < connections; ++i)
    {
        clients.emplace_back(runClient, port, msg_size, &stop, &results[i]);
    }

    std::this_thread::sleep_for(seconds(duration_s));
    stop = true;
    for (auto& c : clients) c.join();
    auto bench_end = Clock::now();

    // ---- Aggregate ----
    double elapsed_s = duration_cast<duration<double>>(bench_end - bench_start).count();
    uint64_t total_req = 0, total_bytes = 0;
    std::vector<double> all_lat;
    for (auto& r : results)
    {
        total_req += r.requests;
        total_bytes += r.bytes;
        all_lat.insert(all_lat.end(), r.latencies_us.begin(), r.latencies_us.end());
    }

    double qps = total_req / elapsed_s;
    double mbps = (total_bytes / (1024.0 * 1024.0)) / elapsed_s;

    std::cout << "=== Cox echo benchmark ===\n";
    std::cout << "Connections:    " << connections << "\n";
    std::cout << "server threads: " << server_threads << "\n";
    std::cout << "msg size:       " << msg_size << "\n";
    std::cout << "duration:       " << elapsed_s <<" s\n";
    std::cout << "requests:       " << total_req << "\n";
    std::cout << "QPS:            " << static_cast<uint64_t>(qps) << " req/s\n";
    std::cout << "throughput:     " << mbps << " MB/s (echo, one direction)\n";
    if (!all_lat.empty())
    {
        std::cout <<"latency p50:      " << percentile(all_lat, 0.50) << " us\n";
        std::cout <<"latency p90:      " << percentile(all_lat, 0.90) << " us\n";
        std::cout <<"latency p99:      " << percentile(all_lat, 0.99) << " us\n";
        std::cout <<"latency max:      " << percentile(all_lat, 1.0) << " us\n";
    }

    stop = true;
    srv_loop->quit();
    server_thread.join();
    return 0;
}