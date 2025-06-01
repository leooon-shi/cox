#pragma once

#include <string>
#include <cstdint>
#include <memory>

namespace logging {

class Formatter;

enum class LogLevel : std::int8_t {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FETAL
};

// class LogEntry {
// public:
//     explicit LogEntry(LogLevel level, Formatter* formatter);
//     explicit LogEntry(LogLevel level);
//     LogEntry();
//     ~LogEntry();
// private:
//     LogLevel level_;
//     Formatter* formatter_;
//     // Timestamp timestamp_;
// };

class Logger {
public:
    //TODO Combine different formatter or multiple formater or destinations.
    // explicit Logger(LogLevel logLevel, Formatter* pretty);
    explicit Logger(LogLevel logLevel);
    Logger();
    virtual ~Logger();

    // void debug(std::string message) { log(LogLevel::DEBUG, std::forward<std::string>(message));}
    // void info(std::string message) { log(LogLevel::INFO, std::forward<std::string>(message));}
    // void warning(std::string message) { log(LogLevel::WARNING, std::forward<std::string>(message));}
    // void error(std::string message) { log(LogLevel::ERROR, std::forward<std::string>(message));}
    // void fetal(std::string message) { log(LogLevel::FETAL, std::forward<std::string>(message));}

    //TODO adding log level control.
    void debug(std::string && message) { log(LogLevel::DEBUG, std::forward<std::string>(message));}
    void info(std::string && message) { log(LogLevel::INFO, std::forward<std::string>(message));}
    void warning(std::string && message) { log(LogLevel::WARNING, std::forward<std::string>(message));}
    void error(std::string && message) { log(LogLevel::ERROR, std::forward<std::string>(message));}
    void fetal(std::string && message) { log(LogLevel::FETAL, std::forward<std::string>(message));}

private:
    void log([[maybe_unused]] LogLevel level, std::string && message);
    // using LogEntries = std::deque<LogEntry>;
    LogLevel level_; //Initialized Log Level
    //TODO performance optimization later.
    //it would be better to use circleBuffer to manage entries.
    // LogEntries logEntries_;
    std::unique_ptr<Formatter> formatter_;
    std::string pid_;
};

} //namespace logging