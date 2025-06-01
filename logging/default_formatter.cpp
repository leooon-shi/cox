#include "formatter.h"
#include "json_formatter.h"

namespace logging
{
std::unique_ptr<Formatter> Formatter::newDefaultFormatter() {
    return std::make_unique<JsonFormatter>();
}
}// namespace logging