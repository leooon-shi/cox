#pragma once

#include <cstdint>
#include <sys/time.h>
#include <string>

namespace logging {
/*
* 
* Timestamp class represents a point in time, typically used for logging purposes.
* timeval defined in <sys/time.h> is a structure that represents time in seconds and microsecond precision.
* struct timeval {
*   time_t      tv_sec;     // seconds since the epoch
*   suseconds_t tv_usec;    // microseconds (0 to 999999)
* };
* struct timeval tv;
* gettimeofday(&tv, NULL); // Get the current time
* 
* timespec is defined in <time.h> and represents time with nanosecond precision.
* struct timespec {
*  time_t   tv_sec;  // seconds since the epoch
*  long     tv_nsec; // nanoseconds (0 to 999999999)
* };
* struct timespec ts;
* clock_gettime(CLOCK_REALTIME, &ts); // Get the current time
* 
* itimerspec is defined in <time.h> and is used for setting and retrieving timer intervals for interval timers.
* struct itimerspec {
*   struct timespec it_interval; // Timer interval
*   struct timespec it_value;    // Initial expiration
* };
* struct itimerspec its;
* its.it_interval.tv_sec = 1; // Set interval to 1 second
* its.it_interval.tv_nsec = 0; // Set interval to 0 nanoseconds
* its.it_value.tv_sec = 5;    // Set initial expiration to 5 seconds
* its.it_value.tv_nsec = 0;   // Set initial expiration to 0 nanoseconds
* timer_t timerid;
* timer_create(CLOCK_REALTIME, NULL, &timerid); // Create a timer
* timer_settime(timerid, 0, &its, NULL); // Set the timer
* timerfd_settime(timerfd, 0, &its, NULL); // Set the timer for a timer file descriptor
* time_t is defined in <time.h> and represents time in seconds since the epoch.
* time_t now = time(NULL); // Get the current time in seconds since the epoch.
*/

class Timestamp {
public:
    Timestamp();
    explicit Timestamp(uint64_t microseconds)
        : microseconds_(microseconds) {}
    
    Timestamp(const Timestamp& other) = default;
    
    Timestamp(Timestamp&& other) noexcept
        : microseconds_(other.microseconds_) {
        other.microseconds_ = 0; // Reset the moved-from object
    }

    // Returns the current time as a Timestamp
    static Timestamp now();

    // Returns the time since epoch in milliseconds
    std::uint64_t millisecondsSinceEpoch() const {
        return microseconds_ / MicroSecondsPerMillisecond;
    }

    // Returns the time since epoch in seconds
    std::uint64_t secondsSinceEpoch() const {
        return microseconds_ / MicroSecondsPerSecond;
    }

    std::string toString() const;
private:
    constexpr static int MicroSecondsPerSecond = 1000 * 1000;
    constexpr static int MicroSecondsPerMillisecond = 1000;
    long microseconds_{0}; // Time in microseconds since epoch
};

} // namespace logging