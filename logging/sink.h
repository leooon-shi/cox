#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace logging
{
// A destination for formatted log lines. The Formatter decides how a record
// LOOKS; the Sink decides where it GOES. Keeping them separate means "JSON to a
// rotating file" needs no new class -- it is a JsonFormatter plus a
// RotatingFileSink.
//
// Reference points: spdlog's sink abstraction (pluggable destinations, multi-sink
// fan-out) and glog's flush-on-fatal behavior.
//
// Thread-safety contract: a Logger may be shared across threads, so
// implementations MUST tolerate concurrent append()/flush(). The synchronous
// sinks here use a mutex; AsyncSink instead hands off to one writer thread.
class Sink {
public:
    virtual ~Sink() = default;

    // Write one formatted line. The sink appends the line terminator, so
    // formatters never emit trailing newlines.
    virtual void append(const std::string& line) = 0;

    // Push buffered data to the OS. Called for Fatal records and on
    // destruction, so the line explaining a crash is not lost (glog's lesson).
    virtual void flush() = 0;

    // ---- Factories ----

    static std::unique_ptr<Sink> newStdoutSink();

    static std::unique_ptr<Sink> newFileSink(const std::string& path,
                                             bool append_mode = true);
    
    // Rolls to "<path>.1", "<path>.2", ... once the active file exceeds
    // max_bytes, keeping at most max_files rolled copies.
    static std::unique_ptr<Sink> newRotatingFileSink(const std::string& path,
                                                     size_t max_bytes,
                                                     size_t max_files);
    
    // Fan out every line to several sinks (e.g. human-readable to stdout AND
    // JSON to a file). Failure of one sink does not stop the others.
    static std::unique_ptr<Sink> newMultiSink(
        std::vector<std::unique_ptr<Sink>> sinks);
};

// Collects lines in memory instead of writing them anywhere. Intended for tests
// and for inspecting output without touching stdout or the filesystem.
class MemorySink final : public Sink {
public:
    void append(const std::string& line) override;
    void flush() override;

    std::vector<std::string> lines() const;
    // All lines joined by '\n' (convenient for substring assertions).
    std::string text() const;
    size_t flushCount() const;
    void clear();
private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    size_t flush_count_{0};
};

}   // namespace logging
