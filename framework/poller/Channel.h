#pragma once
#include <function>

namespace framework {
using EventCallback = std::function<void()>;
class Reactor;
class Channel final {
public:
    void Channel(int fd, Reactor * eventLoop);
    void enableRead(){ events |= ReadEvent; update();}
    void enableWrite(){events |= WriteEvent; update();}
    void disableAll() {events = NoneEvents; update();}
    void update();
    int fd() {return fd_;}
    int events() {return events_;}
    void setRevents(int revents) { revents_ = revents;}
    void setReadCallback(EventCallback callback) {readCallback_ = callback;}
private:
    constexpr ReadEvent = POLLIN | POLLPRI;
    constexpr WriteEvent = POLLOUT;
    constexpr NoneEvents = 0;
    int fd_;     //file descriptor
    int events_; //interested events provided by concrete APPs.
    int revents_; //received events from epoll or poll.
    EventCallback readCallback_;
    Reactor* loop_;
};

}