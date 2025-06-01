#pragma once

#include <functional>
#include <cstdint>
#include "io_event.h"
namespace reactor {
class EventLoop;
class Channel {
public:
    using Callback = std::function<void()>;

    explicit Channel(int fd, EventLoop* loop);
    ~Channel();
    
    int fd() const { return fd_; }

    int events() const { return events_; }
    void setRevents(int revents) { revents_ = revents; }

    void enableRead() { events_ |= IO_READ; update();}
    void disableRead() { events_ &= ~IO_READ; update();}

    void enableWrite() { events_ |= IO_WRITE; update();}
    void disableWrite() { events_ &= ~IO_WRITE; update();}

    void enableError() { events_ |= IO_ERROR; update();}
    void disableError() { events_ &= ~IO_ERROR; update();}

    void disableAll() { events_ = IO_NONE; update();}

    bool isReading() const { return events_ & IO_READ; }
    bool isWriting() const { return events_ & IO_WRITE; }

    void setReadCallback(Callback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(Callback cb) { writeCallback_ = std::move(cb); }
    void setErrorCallback(Callback cb) { errorCallback_ = std::move(cb); }

    void handleEvents();
    void remove();

private:
    void update();

    int fd_;
    int events_;
    int revents_;
    EventLoop* loop_;

    Callback readCallback_;
    Callback writeCallback_;
    Callback errorCallback_;
};
}//namespace reactor