#pragma once

#include "log_record.h"

#include <memory>
#include <string>
#include <utility>

namespace logging
{
class Formatter;
class Sink;

// A leveled logger with a pluggable Formatter (how a line looks), a pluggable
// Sink (where it goes), and optional structured key-value fields.
//
// Level filtering: messages below the configured level are dropped (default
// info, so Debug is suppressed unless enabled).
// 
// Structured fields: pass LogFields to attach context, e.g.
//    log.info("request done", {{"method", "GET"}, {"path", "/x"}, {"status", "200"}});
//
// Sinks: stdout by default. Use Sink::newFileSink / newRotatingFileSink, and
// wrap in an AsyncSink to keep file I/O off an event-loop thread.
//
// Fatal records are flushed immediately (glog behavior), so the line explaining
// a crash survives it.
class Logger {
public:
    explicit Logger(LogLevel logLevel);
    Logger();                               // Defaults to Info + DefaultFormatter + stdout sink
    //Construct with an explicit formatter (e.g., JSON).
    Logger(LogLevel logLevel, std::unique_ptr<Formatter> formatter);
    // Full control: level + formatter + destination.
    Logger(LogLevel logLevel, std::unique_ptr<Formatter> formatter,
            std::shared_ptr<Sink> sink);
    virtual ~Logger();
    
    void setLevel(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    // Swap the destination at runtime (e.g. stdout -> async rotating file once
    // config is loaded). The sink is shared so several Loggers can write to one
    // destination -- important for a single async writer thread process-wide.
    void setSink(std::shared_ptr<Sink> sink);

    // Emit any level >= this immediately rather than relying on sink buffering.
    // Defaults to fatal.
    void setFlushLevel(LogLevel level) { flush_level_ = level; }

    // Plain-message logging.
    void debug(std::string message) { log(LogLevel::Debug,  std::move(message), {});}
    void info(std::string message) { log(LogLevel::Info,  std::move(message), {});}
    void warning(std::string message) { log(LogLevel::Warning,  std::move(message), {});}
    void error(std::string message) { log(LogLevel::Error,  std::move(message), {});}
    void fatal(std::string message) { log(LogLevel::Fatal,  std::move(message), {});}

    // Structured logging ( message + key-value fields).
    void debug(std::string message, LogFields fields) { log(LogLevel::Debug,  std::move(message), std::move(fields));}
    void info(std::string message, LogFields fields) { log(LogLevel::Info,  std::move(message), std::move(fields));}
    void warning(std::string message, LogFields fields) { log(LogLevel::Warning,  std::move(message), std::move(fields));}
    void error(std::string message, LogFields fields) { log(LogLevel::Error,  std::move(message), std::move(fields));}
    void fatal(std::string message, LogFields fields) { log(LogLevel::Fatal,  std::move(message), std::move(fields));}

private:
    void log(LogLevel level, std::string message, LogFields fields);

    LogLevel level_;
    LogLevel flush_level_ { LogLevel::Fatal };
    std::unique_ptr<Formatter> formatter_;
    std::unique_ptr<Sink> sink_;
    std::string pid_;
};

} // namespace logging