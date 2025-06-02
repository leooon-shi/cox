#include "file_opts.h"


#include <unistd.h>
#include <fcntl.h>

namespace file_opts {
    void setNonBlocking(int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

}  // namespace file_opts