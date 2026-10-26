#include "net_runtime.h"

#include <csignal>

namespace net
{
namespace
{
// Runs once at static init or first call: install SIG_IGN for SIGPIPE.
struct IgnoreSigPipe {
    IgnoreSigPipe() { ::signal(SIGPIPE, SIG_IGN); }
};

IgnoreSigPipe g_ignore_sigpipe;

} // namespace

void initNetRuntime() {
    // Touching the global ensures the constructor has run even if the linker
    // would otherwise drop the unused static in some link configurations.
    (void)&g_ignore_sigpipe;
    ::signal(SIGPIPE, SIG_IGN);
}
}   // namespace net
