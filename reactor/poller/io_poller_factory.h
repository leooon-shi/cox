#pragma once
#include <memory>

namespace reactor {
class Poller;

class PollerFactory {
public:
    static std::unique_ptr<Poller> create();
};
} // Reactor