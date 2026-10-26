#pragma once

namespace net {

// Process-wide network runtime initialization.
//
// The most important job is to ignore SIGPIPE: writing to a socket whose peer
// has closed the read end raises SIGPIPE, whose default disposition terminates
// the process. A remote peer could therefore crash the server simply by
// closing a connection at the wrong moment. Ignoring the signal turns such a
// write into a normal EPIPE error that the I/O layer already handles.
//
// This is idempotent and safe to call multiple times. It is invoked
// automatically the first time an EventLoop-based network object is created,
// but may also be called explicitly at program start.

void initNetRuntime();

}   // namespace net
