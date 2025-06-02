// event_mask.h
#pragma once

namespace reactor {

// #if defined(__linux__)
// constexpr int IO_READ  = EPOLLIN;
// constexpr int IO_WRITE = EPOLLOUT;
// #elif defined(__APPLE__) || defined(__FreeBSD__)
// constexpr int IO_READ  = EVFILT_READ;
// constexpr int IO_WRITE = EVFILT_WRITE;
// #endif

enum IOEvent {
    IO_NONE   = 0,
    IO_READ   = 1 << 0,
    IO_WRITE  = 1 << 1,
    IO_ERROR  = 1 << 2
};

inline IOEvent operator|(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<int>(a) | static_cast<int>(b));
}

inline IOEvent operator&(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<int>(a) & static_cast<int>(b));
}

} //namespace reactor