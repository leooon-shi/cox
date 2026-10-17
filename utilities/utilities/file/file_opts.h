#pragma once

#include <utility>

namespace file_opts
{
    void setNonBlocking(int fd);
    std::pair<int, int> createNonBlockingPipe();
    void closePipe(int read_fd, int write_fd);

    // Create a non-blocking, close-on-exec eventfd for cross-thread wakeup of
    // an event loop (Linux-only). Returns the eventfd; throws on failure.
    int createEventfd();
}