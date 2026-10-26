#include "buffer.h"

#include <sys/uio.h>  // readv
#include <unistd.h>

#include <cerrno>

namespace buffer {
ssize_t Buffer::readFd(int fd, int* saved_errno) {
    // Use a stack-based extra buffer so a single readv can pull in a large
    // chunk even when the Buffer's writable region is small, avoiding repeated
    // syscalls. Data that overflows into extrabuf is appended afterwards.
    char extrabuf[65536];
    struct iovec vec[2];
    const size_t writable = writableBytes();

    vec[0].iov_base = beginWrite();
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    // Only use extrabuf when the writable region is smaller than it, to bound
    // stack usage relative to the read size.
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        *saved_errno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        writer_index_ += n;
    } else {
        writer_index_ = data_.size();
        append(extrabuf, n - writable);
    }

    return n;
}

}  // namespace buffer