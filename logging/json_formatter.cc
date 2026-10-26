#include "json_formatter.h"

#include "log_record.h"

#include <cstdio>
#include <string>

namespace logging
{
void JsonFormatter::appendEscaped(std::string& out, const std::string& s)
{
    for (char c : s)
    {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Control character -> \u00XX
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                }
                else 
                {
                    out += c;
                }
        }
    }
}

std::string JsonFormatter::format(const LogRecord& record) const
{
    std::string out;
    out.reserve(96 + record.message.size());
    out += "{\"ts\":\"";
    appendEscaped(out, record.timestamp.toString());
    out += "\",\"level\":\"";
    out += toString(record.level);
    out += "\",\"tid\":\"";
    appendEscaped(out, record.tid);
    out += "\",\"msg\":\"";
    appendEscaped(out, record.message);
    out += "\"";
    // Structured fields as top-level JSON string members.
    for (const auto& [key, value] : record.fields)
    {
        out += ",\"";
        appendEscaped(out, key);
        out += "\":\"";
        appendEscaped(out, value);
        out += "\"";
    }
    out += "}";

    return out;
}
}   // namespace logging
