// event_mask.h
#pragma once

namespace reactor {

enum IOEvent {
    IO_NONE  = 0,
    IO_READ  = 1 << 0,
    IO_WRITE = 1 << 1,
    IO_ERROR = 1 << 2
};

inline IOEvent operator|(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<int>(a) | static_cast<int>(b));
}

inline IOEvent operator&(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<int>(a) & static_cast<int>(b));
}

} // namespace reactor
