#include "timestamp.h"

namespace logging {
Timestamp::Timestamp() : microseconds_(0) {}

Timestamp Timestamp::now() {
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0) {
        return Timestamp();
    }
    // Convert seconds and microseconds to microseconds since epoch
    uint64_t microseconds = static_cast<uint64_t>(tv.tv_sec) * MicroSecondsPerSecond + tv.tv_usec;
    return Timestamp(microseconds);
}

std::string Timestamp::toString() const {
    char buffer[32];
    struct tm tm;
    bzero(&tm, sizeof(tm));
    time_t seconds = static_cast<time_t>(microseconds_ / MicroSecondsPerSecond);
    ::gmtime_r(&seconds, &tm);
    //Microsecond (us) is one-millionth of a second. (10^{-6})
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, microseconds_ % MicroSecondsPerSecond);
    
    return std::string(buffer);
}

} // namespace logging