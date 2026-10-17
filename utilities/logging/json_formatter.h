#pragma once
#include "formatter.h"

namespace logging {
class JsonFormatter final : public Formatter
{
public:
    JsonFormatter() = default;
    ~JsonFormatter() override;
private:
    void format() override;
};
}