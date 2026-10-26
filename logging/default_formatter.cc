#include "default_formatter.h"

#include "formatter.h"
#include "json_formatter.h"
#include "log_record.h"

#include <string>

namespace logging
{
std::string DefaultFormatter::format(const LogRecord& record) const {
    std::string out;
    out.reserve(64 + record.message.size());
    out += "[";
    out += record.tid;
    out += "] ";
    out += record.timestamp.toString();
    out += " ";
    out += toString(record.level);
    out += " ";
    out += record.message;
    // Append structured fields as space-separated key=value pairs.
    for (const auto& [ key, value] : record.fields)
    {
        out += " ";
        out += key;
        out += "=";
        out += value;
    }

    return out;
}

// Factories (kept here since this TU already depends on both formatters).
std::unique_ptr<Formatter> Formatter::newDefaultFormatter() 
{
    return std::make_unique<DefaultFormatter>();
}

std::unique_ptr<Formatter> Formatter::newJsonFormatter() {
    return std::make_unique<JsonFormatter>();
}

}   // namespace logging
