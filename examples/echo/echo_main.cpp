#include "event_loop.h"
#include <iostream>
#include <string>
#include <map>
#include <thread>

int main() {
    reactor::EventLoop* loop(new reactor::EventLoop());
    std::map<std::string, std::string> items {
        {"hello", " world"},
        {"Name", " Leon"},
        {"Genlendor", " Female"},
        {"Hobbies", " Walking"},
        {"Job", " SW Developer"}
    };

    std::thread thread1([&]() {
        std::cout <<"thread 1 " << std::this_thread::get_id() << std::endl;
        for(auto & [key, value] : items)
        {
            std::this_thread::sleep_for(std::chrono::duration(std::chrono::seconds(1)));
            loop->runInLoop([&](){std::cout <<key <<":" << value << " from thread 1 into loop thread " << std::this_thread::get_id() <<std::endl;});
        }
    });

    std::thread thread2([&]() {
        std::cout <<"thread 2 " << std::this_thread::get_id() << std::endl;
        for( auto i = 0; i < 5; ++i)
        {
            for(auto & [key, value] : items)
            {
                loop->runInLoop([&](){std::cout <<key <<":" << value << " from thread 2 into loop thread " << std::this_thread::get_id() <<std::endl;});
            }
        }
    });

    std::thread stopThr([&](){
        std::this_thread::sleep_for(std::chrono::duration(std::chrono::seconds(7)));
        loop->quit();
        delete loop;
        loop=nullptr;
    });

    loop->loop();
    std::cout <<"before join " <<std::this_thread::get_id() << std::endl;
    thread1.join();
    thread2.join();
    stopThr.join();
    std::cout <<"after join" << std::endl;

    return 0;
}