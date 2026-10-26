#include "timestamp.h"

#include <cstdio>   // snprintf
#include <cstring>  // memset
#include <ctime>    // struct tm, gmtime_r, time_t

namespace logging {

Timestamp::Timestamp() : microseconds_(0) {}

Timestamp Timestamp::now() {
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0) {
        return Timestamp();
    }
    // Convert seconds and microseconds to microseconds since epoch
    uint64_t microseconds =
        static_cast<uint64_t>(tv.tv_sec) * MicroSecondsPerSecond + tv.tv_usec;
    return Timestamp(microseconds);
}

std::string Timestamp::toString() const {
    // "YYYY-MM-DD HH:MM:SS.ffffff" needs 27 bytes including the NUL, but the
    // compiler cannot prove tm_year is bounded, so size generously rather than
    // risk truncation on an absurd input.
    char buffer[64];
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    time_t seconds = static_cast<time_t>(microseconds_ / MicroSecondsPerSecond);
    ::gmtime_r(&seconds, &tm);
    // Microsecond (us) is one-millionth of a second (10^-6). Cast the fraction
    // explicitly: it is unsigned, so print it with an unsigned specifier.
    auto micros =
        static_cast<unsigned long>(microseconds_ % MicroSecondsPerSecond);
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%06lu",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, micros);

    return std::string(buffer);
}

}  // namespace logging
