#pragma once

#include "sink.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace logging
{

// AsyncSink moves file I/O off the calling thread. This is the one piece of the
// logging module that the reactor genuinely needs: a synchronous write() on an
// event-loop thread stalls every connection on that loop (see
// insights/features/reactor-blocking-work.md), so the loop must hand the line
// off and return immediately.
//
// Design (muduo's AsyncLogging): DOUBLE BUFFERING, not a per-message handoff.
// Producers append into a current buffer under a very short lock. The writer
// thread SWAPS in an empty buffer and then does its I/O entirely outside the
// lock, so producer cost is an append plus a swap -- not a syscall, and never a
// wait on disk.
//
// OVERFLOW POLICY: the queue is BOUNDED and DROPS when full, incrementing a
// counter. Blocking a producer would defeat the purpose (a stalled disk would
// stall the event loop, which is exactly the failure being avoided), and
// unbounded growth turns a logging burst into OOM. This mirrors Cox's existing
// load-shedding posture (ThreadPool::tryRun, accept-rate shedding) and Fluent
// Bit's mem_buf_limit: shed, and account for what was shed rather than hiding
// it. droppedCount() exposes the tally, and a summary line is emitted when
// writing resumes so drops are visible in the log itself.
class AsyncSink final : public Sink {
public:
    // max_buffered_lines bounds the lines held across both buffers.
    // flush_interval_ms bounds how long a line can sit unwritten on an idle
    // system (so a low-traffic service still gets timely logs).
    explicit AsyncSink(std::unique_ptr<Sink> downstream,
                       size_t max_buffered_lines = 8192,
                       uint32_t flush_interval_ms = 500);
    ~AsyncSink() override;

    AsyncSink(const AsyncSink&) = delete;
    AsyncSink& operator=(const AsyncSink&) = delete;

    // Never blocks on I/O. Drops (and counts) if the buffer is full.
    void append(const std::string& line) override;

    // Blocks until everything queued so far has been handed downstream and
    // flushed. Used for Fatal records and at shutdown.
    void flush() override;

    // Lines discarded because the buffer was full.
    uint64_t droppedCount() const { return dropped_.load(); }

    // Stop the writer thread after draining. Idempotent; called by the dtor.
    void stop();

private:
    void writerThread();

    std::unique_ptr<Sink> downstream_;
    size_t max_lines_;
    uint32_t flush_interval_ms_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable drained_;

    std::vector<std::string> current_;  // Producers append here
    std::vector<std::string> writing_;  // Writer drains this (swapped in)
    std::atomic<uint64_t> dropped_{0};
    uint64_t reported_drops_{0};        // Already announced in the log
    bool running_{false};
    std::thread thread_;
};

} // namespace logging