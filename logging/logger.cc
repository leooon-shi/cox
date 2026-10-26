#include "logger.h"

#include "formatter.h"
#include "log_record.h"
#include "sink.h"
#include "thread/current_thread.h"
#include "timestamp.h"

#include <memory>
#include <mutex>
#include <utility>

namespace logging {
namespace {
// Process-wide default sink, so every default-constructed Logger (the common
// `static logging::Logger` per translation unit) shares ONE stdout destination
// rather than each holding its own. That keeps output interleaving sane and
// means a single async writer thread can serve the whole process.
std::shared_ptr<Sink>& defaultSink() {
    static std::shared_ptr<Sink> sink = Sink::newStdoutSink();
    return sink;
}
}  // namespace

Logger::Logger(LogLevel level)
    : level_(level),
      formatter_(Formatter::newDefaultFormatter()),
      sink_(defaultSink()),
      pid_(current_thread::tidStr()) {}

Logger::Logger()
    : level_(LogLevel::Info),
      formatter_(Formatter::newDefaultFormatter()),
      sink_(defaultSink()),
      pid_(current_thread::tidStr()) {}

Logger::Logger(LogLevel level, std::unique_ptr<Formatter> formatter)
    : level_(level),
      formatter_(std::move(formatter)),
      sink_(defaultSink()),
      pid_(current_thread::tidStr()) {}

Logger::Logger(LogLevel level, std::unique_ptr<Formatter> formatter,
               std::shared_ptr<Sink> sink)
    : level_(level),
      formatter_(std::move(formatter)),
      sink_(std::move(sink)),
      pid_(current_thread::tidStr()) {}

Logger::~Logger() = default;

void Logger::setSink(std::shared_ptr<Sink> sink) {
    sink_ = std::move(sink);
}

void Logger::log(LogLevel level, std::string message, LogFields fields) {
    // Level filter: drop anything below the configured minimum. Done BEFORE
    // formatting so a suppressed record costs nothing but a comparison.
    if (static_cast<int>(level) < static_cast<int>(level_)) {
        return;
    }
    LogRecord record{level, Timestamp::now(), pid_, std::move(message),
                     std::move(fields)};
    sink_->append(formatter_->format(record));
    // Flush high-severity records immediately: a Fatal line must survive the
    // crash that follows it (glog behavior).
    if (static_cast<int>(level) >= static_cast<int>(flush_level_)) {
        sink_->flush();
    }
}

}  // namespace logging
