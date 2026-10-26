// Micro-benchmark for buffer::Buffer append/retrieve throughput.

#include "buffer.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

using namespace std::chrono;

int main()
{
    constexpr int kIters = 5'000'000;
    const std::string chunk(256, 'x');

    // Append + full retrieve cycle (typical read-consume pattern).
    auto t0 = steady_clock::now();
    uint64_t total = 0;
    for (int i = 0; i < kIters; ++i)
    {
        buffer::Buffer buf;
        buf.append(chunk);
        buf.append(chunk);
        total += buf.readableBytes();
        buf.retrieveAll();
    }

    auto t1 = steady_clock::now();
    double s = duration_cast<duration<double>>(t1 - t0).count();

    std::cout << "=== Cox buffer benchmark ===\n";
    std::cout << "iterations:  " << kIters << " (2x256B append + retrieveAll)\n";
    std::cout << "elapsed:     " << s << " s\n";
    std::cout << "opt/s:       " << static_cast<uint64_t>(kIters / s) << "\n";
    std::cout << "throughput:  " 
              << (static_cast<double>(total) / (1024.0 * 1024.0)) / s 
              << " MB/s\n";
    
    // Growth path: repeatedly append into one buffer without retrieving.
    auto t2 = steady_clock::now();
    buffer::Buffer big;
    for(int i = 0; i < kIters; ++i)
    {
        big.append(chunk);
        if (big.readableBytes() > (8u << 20)) big.retrieveAll();  // cap ~8MB
    }
    auto t3 = steady_clock::now();
    double s2 = duration_cast<duration<double>>(t3 - t2).count();
    std::cout << "append-heavy:" << static_cast<uint64_t>(kIters / s2)
              << " appends/s\n";
    return 0;
}
