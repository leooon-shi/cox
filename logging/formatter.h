#pragma once
#include <memory>

namespace logging
{
class Formatter {
public:
    virtual ~Formatter() = 0;

    virtual void format() = 0;
    
    static std::unique_ptr<Formatter> newDefaultFormatter();
};

}