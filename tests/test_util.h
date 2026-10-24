#pragma once

// Small helpers shared by the socket-based tests.
// WHY THIS EXISTS: glibc marks write()/read()/system() warn_unused_result, and
// optimized GCC builds turn an ignored return into a -Werror-unused-result
// failure. Tests legitimately drive I/O just to set up a scanrio, where the
// exact byte count is not the thing under test -- but silently discarding the
// result is still wrong, because a failed write would make the surrounding
// assertions meaningless rather than failing loudly.
//
// These wrappers make the intent explicit at each call site:
//  sendAll()   -- Write and REQUIRE it fully succeed (setup that must work)
//  drainSome() -- Read whatever is available, tolerating none (teardown/drain)
//  runCmd()    -- RUn a shell command and return its status for checking

#include <unistd.h>

#include <cstdlib>
#include <string>

namespace test_util
{
// Write the whole buffer. Returns true only if every byte was accepted.
inline bool sendAll(int fd, const void* data, size_t len)
{
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = ::write(fd, p + sent, len - sent);
        if ( n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline bool sendAll(int fd, const std::string& s)
{
    return sendAll(fd, s.data(), s.size());
}

// Read whatever is currently available. Returns the byte count (0 or -1 is not
// an error here -- callers use this to drain, not to assert content).
inline ssize_t drainSome(int fd, char* buf, size_t len)
{
    return ::read(fd, buf, len);
}

// Run a shell command, returning its exit status ( 0 == success).
inline int runCmd(const std::string& cmd)
{
    return std::system(cmd.c_str());
}
} // namespace test_util

