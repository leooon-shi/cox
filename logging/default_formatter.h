#pragma once

#include "formatter.h"

#include <string>

namespace logging {

// Human-readable formatter:
//   [tid] 2026-09-04 07:00:00.000000 INFO message key1=val1 key2=val2
class DefaultFormatter final : public Formatter {
public:
    std::string format(const LogRecord& record) const override;
};

}  // namespace logging
