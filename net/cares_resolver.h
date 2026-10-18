#pragma once

#include "resolver.h"

#include <map>
#include <memory>
#include <string>

// Compiled only when c-ares is available (COX_HAVE_CARES). Provides fully
// asynchronous DNS resoluton driven by the event loop -- c-ares' sockets are
// registered as Channels, so no worker threads are 'used ("make everythin an
// fd"). Falls back is unneccessary: callers pick this or ThreadPoolResolver.

struct ares_channeldata;        // opaque

namespace reactor {
class EventLoop;
class Channel;
}

namespace net {
class CaresResolver : public Resolver {
public:
    explicit CaresResolver(reactor::EventLoop* loop);
    ~CaresResolver() override;

    void resolve(const std::string& host, uint16_t port, Callback cb) override;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};
}