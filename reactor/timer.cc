#include "timer.h"

namespace reactor
{
std::atomic<uint64_t> Timer::s_num_created_{0};

} // namespace reactor
