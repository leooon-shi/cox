#pragma once

#include "timestamp.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace logging
{

enum class LogLevel : std::int8_t {
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

const char* toString(LogLevel level);

// A structured log field: a key with a string value. Non-string values are
// stringified by the caller (helpers provided on Logger).

using LogField = std::pair<std::string, std::string>;
using LogFields = std::vector<LogField>;

// A single log event handed to a Formatter. Carries the severity, timestamp,
// thread id, message, and optional structured key-value fields. Formatters turn
// this into the final output line (human-readable or JSON).
struct LogRecord {
    LogLevel level;
    Timestamp timestamp;
    std::string tid;        // thread id string
    std::string message;
    LogFields fields;       // structured context (may be empty)
};
} // namespace logging