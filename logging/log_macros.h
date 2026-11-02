#pragma once

// Convenience macros over logging::Logger (Option A: thin wrappers, not a
// stream redesign). They solve three things the bare API does not:
//
//   1. Lazy evaluation. The level is checked BEFORE the message string is
//      built, so a suppressed record (e.g. COX_LOG_DEBUG when level is Info)
//      costs one comparison instead of a full concatenation. This closes the
//      "arguments are evaluated even when dropped" gap noted in the roadmap.
//   2. Automatic `__func__` prefix. Call sites no longer hand-type
//      "ClassName::method " into every message (which drifts from reality
//      after a rename). Only the function name is added -- deliberately NOT
//      __FILE__:__LINE__, which is redundant with it.
//   3. Less boilerplate. COX_LOG_DEFINE() replaces the per-translation-unit
//      anonymous-namespace `log()` accessor that was copy-pasted across ~14
//      files.
//
// Usage:
//   #include "log_macros.h"
//   namespace net {
//   COX_LOG_DEFINE();                 // defines a file-local log() accessor
//
//   void Foo::bar() {
//       COX_LOG_ERROR("accept failed: " + std::string(std::strerror(errno)));
//       COX_LOG_INFO("connected", {{"peer", addr.toIpPort()}});
//   }
//   }  // namespace net
//
// The message expression is whatever the Logger accepts (a std::string), so
// existing `"a" + b + std::to_string(n)` call sites move over unchanged.

#include "logger.h"

#include <string>
#include <utility>

// Define a file-local logger accessor. Place once per .cc, inside the file's
// namespace (or an anonymous namespace). The logger is a function-local static
// so construction is thread-safe and lazy.
#define COX_LOG_DEFINE()                 \
    namespace {                          \
    ::logging::Logger& log() {           \
        static ::logging::Logger logger; \
        return logger;                   \
    }                                    \
    }  // namespace

// Core macro: guard on level, then emit "<func>: <message>". Wrapped in
// do/while(0) so it is a single statement and composes with if/else without
// braces surprises.
#define COX_LOG_EMIT(LEVEL_ENUM, METHOD, MESSAGE)                      \
    do {                                                               \
        ::logging::Logger& _cox_log = log();                           \
        if (_cox_log.level <= ::logging::LogLevel::LEVEL_ENUM) {       \
            _cox_log.METHOD(std::string(__func__) + ": " + (MESSAGE)); \
        }                                                              \
    } while (0)

// Same, with structured LogFields.
#define COX_LOG_EMIT_F(LEVEL_ENUM, METHOD, MESSAGE, FIELDS)           \
    do {                                                              \
        ::logging::Logger& _cox_log = log();                          \
        if (_cox_log.level <= ::logging::LogLevel::LEVEL_ENUM) {      \
            _cox_log.METHOD(std::string(__func__) + ": " + (MESSAGE), \
                            (FIELDS));                                \
        }                                                             \
    } while (0)

#define COX_LOG_DEBUG(MESSAGE) COX_LOG_EMIT(Debug, debug, MESSAGE)
#define COX_LOG_INFO(MESSAGE) COX_LOG_EMIT(Info, info, MESSAGE)
#define COX_LOG_WARNING(MESSAGE) COX_LOG_EMIT(Warning, warning, MESSAGE)
#define COX_LOG_ERROR(MESSAGE) COX_LOG_EMIT(Error, error, MESSAGE)
#define COX_LOG_FATAL(MESSAGE) COX_LOG_EMIT(Fatal, fatal, MESSAGE)

#define COX_LOG_DEBUG_F(MESSAGE, FIELDS) \
    COX_LOG_EMIT_F(Debug, debug, MESSAGE, FIELDS)
#define COX_LOG_INFO_F(MESSAGE, FIELDS) \
    COX_LOG_EMIT_F(Info, info, MESSAGE, FIELDS)
#define COX_LOG_WARNING_F(MESSAGE, FIELDS) \
    COX_LOG_EMIT_F(Warning, warning, MESSAGE, FIELDS)
#define COX_LOG_ERROR_F(MESSAGE, FIELDS) \
    COX_LOG_EMIT_F(Error, error, MESSAGE, FIELDS)
#define COX_LOG_FATAL_F(MESSAGE, FIELDS) \
    COX_LOG_EMIT_F(Fatal, fatal, MESSAGE, FIELDS)
