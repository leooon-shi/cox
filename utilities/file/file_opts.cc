#include "file_opts.h"


#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>
#include <cstring>
#include <string>
namespace file_opts {
void setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::pair<int, int> createNonBlockingPipe() {
    int fds[2];
    if (::pipe(fds) != 0) {
        throw std::runtime_error("pipe() failed: " + std::string(std::strerror(errno)));
    }
    
    setNonBlocking(fds[0]);
    setNonBlocking(fds[1]);
    
    return {fds[0], fds[1]};
}

}  // namespace file_opts