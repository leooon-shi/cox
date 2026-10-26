#include "sink.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace logging
{
namespace
{
// --- stdout ---

// Writes to stdout via a FILE*. Uses the C stdio layer rather than std::cout so
// buffering and flushing are explicit and cheap.

class StdoutSink final : public Sink 
{
public:
    void append(const std::string& line) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
    }

    void flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::fflush(stdout);
    }
private:
    std::mutex mutex_;
};


// ---- plain file ----

class FileSink final : public Sink {
public:
    FileSink(const std::string& path, bool append_mode)
    {
        fp_ = std::fopen(path.c_str(), append_mode ? "ae" : "we");
    }

    ~FileSink() override {
        if (fp_)
        {
            std::fflush(fp_);
            std::fclose(fp_);
        }
    }

    void append(const std::string& line) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fp_) return; // open failed; drop rather than crash

        std::fwrite(line.data(), 1, line.size(), fp_);
        std::fputc('\n', fp_);
    }

    void flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fp_) std::fflush(fp_);
    }

private:
    std::mutex mutex_;
    std::FILE* fp_{nullptr};
};

// ---- rotating file ----

// Rolls the active file once it exceeds max_bytes: <path>.N-1 -> <path>.N, 
// <path> -> <path>.1, then reopens <path> empty. Bounds total disk use to 
// roughly max_bytes * (max_files + 1) so logging cannot fill a volume.
class RotatingFileSink final : public Sink {
public:
    RotatingFileSink(const std::string& path, size_t max_bytes,
                     size_t max_files)
    : path_(path),
      max_bytes_(max_bytes == 0 ? 1 : max_bytes),
      max_files_(max_files)
    {
        open();
    }

    ~RotatingFileSink() override { close(); };

    void append(const std::string& line) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fp_) return;

        // Rotate BEFORE writing when this line would exceed the cap, so the
        // limit is never overshot by more than one line.
        if (written_ + line.size() +1 > max_bytes_ && written_ > 0)
        {
            rotate();
        }

        std::fwrite(line.data(), 1, line.size(), fp_);
        std::fputc('\n', fp_);
        written_ += line.size() + 1;
    }

    void flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fp_) std::fflush(fp_);
    }
private:
    void open() {
        fp_ = std::fopen(path_.c_str(), "ae");
        written_ = 0;
        if (fp_) {
            std::fseek(fp_, 0, SEEK_END);
            long pos = std::ftell(fp_);
            if (pos > 0) written_ = static_cast<size_t>(pos);
        }
    }

    void close() {
        if (fp_)
        {
            std::fflush(fp_);
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    static std::string rolled(const std::string& base, size_t n) {
        return base + "." + std::to_string(n);
    }

    void rotate()
    {
        close();
        if (max_files_ == 0)
        {
            // No history kept: just truncate.
            std::FILE* t = std::fopen(path_.c_str(), "we");
            if (t) std::fclose(t);
            open();
            return;
        }

        // Drop the oldest, then shift each rolled file up by one.

        std::remove(rolled(path_, max_files_).c_str());
        for (size_t i = max_files_; i > 1; --i)
        {
            std::rename(rolled(path_, i - 1).c_str(), rolled(path_, i).c_str());
        }
        std::rename(path_.c_str(), rolled(path_, 1).c_str());
        open();
    }

    std::mutex mutex_;
    std::string path_;
    size_t max_bytes_;
    size_t max_files_;
    std::FILE* fp_{nullptr};
    size_t written_{ 0 };
};


// ---- fan-out ----

// Writes every line to all downstream sinks (spdlog-style multi-sink), e.g.
// Human-readable to stdout and JSON to a file in the same process.
class MultiSink final : public Sink {
public:
    explicit MultiSink(std::vector<std::unique_ptr<Sink>> sinks)
    : sinks_(std::move(sinks))
    {
    }
    void append(const std::string& line) override {
        for (auto& s : sinks_) s->append(line);
    }

    void flush() override {
        for ( auto& s : sinks_) s->flush();
    }
private:
    // Each downstream sink does its own locking, so no mutex is needed here.
    std::vector<std::unique_ptr<Sink>> sinks_;
};

} // namespace

std::unique_ptr<Sink> Sink::newStdoutSink() {
    return std::make_unique<StdoutSink>();
}

std::unique_ptr<Sink> Sink::newFileSink(const std::string& path, 
                                        bool append_mode)
{
    return std::make_unique<FileSink>(path, append_mode);
}

std::unique_ptr<Sink> Sink::newRotatingFileSink(const std::string& path,
                                                size_t max_bytes,
                                                size_t max_files) {
    return std::make_unique<RotatingFileSink>(path, max_bytes, max_files);
}

std::unique_ptr<Sink> Sink::newMultiSink(
    std::vector<std::unique_ptr<Sink>> sinks)
{
    return std::make_unique<MultiSink>(std::move(sinks));
}

// ---- MemorySink (test/inspection sink) ----
void MemorySink::append(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(line);
}

void MemorySink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++flush_count_;
}

std::vector<std::string> MemorySink::lines() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
}

std::string MemorySink::text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (const auto& l : lines_)
    {
        if (!out.empty()) out += "\n";
        out += l;
    }
    return out;
}

size_t MemorySink::flushCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_count_;
}

void MemorySink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    flush_count_ = 0;
}

} // namespace logging