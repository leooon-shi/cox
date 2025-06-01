#pragma once

#include "likely.h"

#include <sys/types.h>

namespace current_thread {
    //Declare thread-local storage for thread id.
    extern thread_local pid_t tid_;
    //Normally, /proc/sys/kernel/pid_max is 32768 (2^15) on Linux. 4194304(2^22) on modern systems.
    // So, 32 bytes is enough to hold a 5-digit pid and a null terminator.
    // But, we use 32 bytes to be safe for future expansion.
    // Also, it is enough to hold a 10-digit pid and a null terminator.
    // Note: pid_t is defined as int in POSIX, so it can hold values up to 32767 on 16-bit systems.
    // But, on modern systems, pid_t is defined as int32_t, so it can hold values up to 2147483647.
    // Note: The tid_str_ is used to hold the string representation of the thread id.
    // It is used for logging purposes, so it is not a performance-critical part of the code.
    // The tid_str_len_ is used to hold the length of the tid_str_.
    // // It is used to avoid unnecessary string operations.
    extern thread_local char tid_str_[32];

    pid_t getTid();
    void cacheTid();

    inline pid_t tid() {
        if(UNLIKELY(tid_ == 0)) {
            // Cache the thread id only once.
            // This is a performance optimization to avoid calling syscall repeatedly.
            cacheTid();
        }

        return tid_;
    }

    inline const char* tidStr()
    {
        if(UNLIKELY(tid_ == 0)) {
            // Cache the thread id only once.
            // This is a performance optimization to avoid calling syscall repeatedly.
            cacheTid();
        }
        // return std::string(tid_str_, tid_str_len_);
        return tid_str_;
    }
} // namespace current_thread