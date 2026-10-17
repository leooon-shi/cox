#include "async_sink.h"

#include <chrono>
#include <string>
#include <utility>

namespace logging {
AsyncSink::AsyncSink(std::unique_ptr<Sink> downstream, size_t max_buffered_lines,
                    uint32_t flush_interval_ms)
: downstream_(std::move(downstream)),
 max_lines_(max_buffered_lines == 0 ? 1 : max_buffered_lines),
 flush_interval_ms_(flush_interval_ms)
{
    running_ = true;
    thread_ = std::thread([this]{ writerThread(); });
}

AsyncSink::~AsyncSink() { stop(); }

void AsyncSink::append(const std::string& line)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        //Bounded: shed rather than block a (possibly event-loop) producer.
        if (current_.size() + writing_.size() >= max_lines_)
        {
            dropped_.fetch_add(1);
            return;
        }
        current_.push_back(line);
    }
    not_empty_.notify_one();
}

void AsyncSink::flush()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_)
    {
        lock.unlock();
        downstream_->flush();
        return;
    }

    //Wake the writer and wait until both buffers are empty.
    not_empty_.notify_one();
    drained_.wait(lock, [this]{
        return (current_.empty() && writing_.empty()) || !running_;
    });

    lock.unlock();
    downstream_->flush();
}

void AsyncSink::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    not_empty_.notify_all();
    drained_.notify_all();
    if (thread_.joinable()) thread_.join();

    // Drain anything the writer did not get to, so shutdown loses nothing.
    std::vector<std::string> leftover;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        leftover.swap(writing_);
        for (auto& l : current_) leftover.push_back(std::move(l));
        current_.clear();
    }

    for (auto& line : leftover) downstream_->append(line);
    downstream_->flush();
}

void AsyncSink::writerThread()
{
    while(true)
    {
        std::vector<std::string> batch;
        uint64_t drops_to_report = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (current_.empty() && running_)
            {
                // Wait for work, but wake periodically so a low-traffic
                // service still sees its lines promptly.
                not_empty_.wait_for(lock, std::chrono::milliseconds(flush_interval_ms_),
                [this]{
                    return !current_.empty() || !running_;
                });
            }

            if (!running_ && current_.empty())
            {
                drained_.notify_all();
                return;
            }

            // The SWAP: take the producers' buffer and give them an empty one.
            // All I/O below happens with the lock released.
            current_.swap(writing_);
            batch.swap(writing_);

            uint64_t total = dropped_.load();
            if (total > reported_drops_)
            {
                drops_to_report = total - reported_drops_;
                reported_drops_ = total;
            }
        }

        for (const auto& line : batch) downstream_->append(line);
        //Make shedding visible in the log itself, not just via the counter.
        if (drops_to_report > 0)
        {
            downstream_.append(
                "AsyncSink: dropped " + std::to_string(drops_to_report) +
                " log line(s) -- buffer full");
        }

        if (!batch.empty()) downstream_->flush();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_.empty() && writing_.empty()) drained_.notify_all();
        }
    }
}

} // namespace logging