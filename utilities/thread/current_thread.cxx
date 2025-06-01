#include "current_thread.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <iomanip> // for std::setw and std::setfill
#include <sstream> // for std::ostringstream

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <pthread.h>
#endif

#if defined(__GLIBC__) && (__GLIBC__ >= 2 && __GLIBC_MINOR__ >= 30)
#define USE_GETTID
#endif


namespace current_thread {

    thread_local pid_t tid_ = 0; // Thread-local storage for thread ID
    thread_local char tid_str_[32] = {0}; // Thread-local storage for thread ID string
    
    pid_t getTid() {
    #ifdef USE_GETTID
        return ::gettid();
    #elif defined(__linux__)
        // On Linux, we can use the syscall interface to get the thread ID.
        return static_cast<pid_t>(::syscall(SYS_gettid));
    #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        uint64_t tid;
        ::pthread_threadid_np(NULL, &tid);
        return static_cast<pid_t>(tid);
    
    #endif
    }
    
    void cacheTid() {
        tid_ = getTid();
        // std::cout << "Thread ID cached: " << tid_ << std::endl;
    #if defined (__APPLE__)
        std::ostringstream oss;
        oss << std::setw(7) << std::setfill('0') << tid_;
        std::string tid_str = oss.str();
        auto tid_str_len = tid_str.length();
        std::copy(tid_str.begin(), tid_str.end(), tid_str_);
        tid_str_[tid_str_len] = '\0'; // Null-terminate the string
    // CXX20
    // std::string tid_str = std::format("{:07}", tid_); // Fixed width of 5, padded with '0'

    #else
        auto tid_str_len = std::snprintf(tid_str_, sizeof(tid_str_), "%-7d", tid_);
        tid_str_[tid_str_len] = '\0'; // Null-terminate the string
    #endif
    }
    } // namespace current_thread