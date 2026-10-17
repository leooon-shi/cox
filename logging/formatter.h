#pragma once

#include <memory>
#include <string>

namespace logging {
struct LogRecord;

// Turns a LogRecord into an output line. Implementations: DefaultFormatter
// (human-readable) and JsonFormatter (structured JSON for log aggregation).
class Formatter {
public:
    virtual ~Formatter() = default;

    // Format a record into a single line (no trailing newline; the sink adds
    // it). Must be thread-safe if the logger is shared across threads
    // (implementations here are stateless, hence safe).
    virtual std::string format(const LogRecord& record) const = 0;

    // Convenience factories.
    static std::unique_ptr<Formatter> newDefaultFormatter();
    static std::unique_ptr<Formatter> newJsonFormatter();
};
} // namespace logging