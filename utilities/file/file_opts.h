#pragma once

#include <utility>
namespace file_opts{
    void setNonBlocking(int fd);
    std::pair<int, int> createNonBlockingPipe();
    void closePipe(int read_fd, int write_fd);
}