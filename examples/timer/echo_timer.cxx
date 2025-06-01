#include <memory>
#include <iostream>
#include <sys/timerfd.h>

#include "cox/framework/reactor/Reactor.h"
#include "cox/framework/poller/Poller.h"
#include "cox/framework/Poller/Channel.h"

class EventHandler final
{
public:
    EventHandler(framework::Reactor* eventLoop)
    :timerfd_(createTimerfd()),
    loop_(eventLoop),
    chan_(std::make_unique<framework::Channel>(fd, loop_))
    {
        std::cout <<"Construct Event Hanlder" << std::endl;
        setReadCallback(std::bind(&EventHandler::onTimeout, this));
    }
    void callAfter(in seconds)
    {
        setTimer(seconds);
    }
    
private:
    int createTimerfd()
    {
        int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timerfd < 0)
        {
            std::error << "create timerfd failed. " << std::endl;
        }
        return timerfd;
    }

    int setTimer(int seconds)
    {
        struct itimerspec newValue {0};
        struct itimerspec oldValue {0};
        newValue.it_value.tv_sec = seconds;
        int ret = ::timerfd_settime(timerfd_, 0, &newValue, &oldValue);
        if (ret < 0)
        {
            std::cout <<"set timer error" << std::endl;
        }
    }

    void onTimeout()
    {
        uint64_t ret {0};
        int nbytes = ::read(timerfd_, &ret, sizeof ret);
        if (nbytes != sizeof ret)
        {
            std::cout << "read buffer get partial of uint64_t." << std::endl;
        }

        std::cout <<"timeout occurs" << std::endl;
        loop_.stop();
        std::cout <<"stop event loop." << std::endl;
    }

    const int timerfd_;
    std::unique_ptr<framework::Channel> chan_;
    framework::Reactor* loop_;
};

int main()
{
    framework::Reactor eventLoop;
    EventHandler evHandler(&eventLoop);
    evHandler.callAfter();
    eventLoop.loop();
    return 0;
}