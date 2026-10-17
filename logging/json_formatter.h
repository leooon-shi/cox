#pragma once

#include "formatter.h"

#include <string>

namespace logging {

// Structured JSON formatter, one object per line (JSON lines), suitable for log
// aggregation (ELK, Loki, CloudWatch)
//      {"ts": "...", "level": "INFO", "tid":"123", "msg":"...", "key1":"val1"}

class JsonFormatter final : public Formatter
{
public:
    std::string format(const LogRecord& record) const override;

private:
    static void appendEscaped(std::string& out, const std::string& s);
};
} // namespace logging