// LogLevel string mapping. The formatter base is header-only (default dtor);
// factories live in default_formatter.cc
#include "log_record.h"

namespace logging {
const char* toString(LogLevel level)
{
    switch(level)
    {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARNING";
        case LogLevel::Error:   return "ERROR";
        case LogLevel::Fatal:   return "FATAL";
    }
    return "UNKNOWN";
}

} // namespace logging