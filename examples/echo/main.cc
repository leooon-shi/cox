#include "cow/reactor/EventLoop.h"

int main()
{
    cow::framework::EventLoop evLoop;
    
    evLoop.loop();
    return 0;
}