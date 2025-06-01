#include "logger.h"
#include "formatter.h"
#include "timestamp.h"
#include "thread/current_thread.h"

#include <iostream>
#include <chrono>

namespace logging {
// Logger::Logger(LogLevel level, Formatter* formatter)
// :level_(level), formatter(formatter) {

// }

Logger::Logger(LogLevel level)
:level_(level),
formatter_(Formatter::newDefaultFormatter()),
pid_(current_thread::tidStr())
{
}

Logger::Logger()
:level_(LogLevel::INFO),
formatter_(Formatter::newDefaultFormatter()),
pid_(current_thread::tidStr())
{

}

Logger::~Logger()
{
}

void Logger::log(LogLevel level, std::string && message)
{
    // const auto now = std::chrono::utc_clock::now();
    // const auto now = std::chrono::steady_clock::now();
    std::string now = Timestamp::now().toString();
    std::cout <<"[" << pid_ << "] " << now << " " << message << std::endl;
}

} //namespace logging