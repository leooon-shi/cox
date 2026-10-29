// DNS resolver tests: ThreadPoolResolver (always) and CaresResolver (when
// c-ares is available). Uses localhost/127.0.0.1 to avoid external network.

#include "resolver.h"
#include "event_loop.h"
#include "inet_address.h"
#include "thread_pool.h"

#ifdef COX_HAVE_CARES
#include "cares_resolver.h"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace net;

TEST(ResolverTest, ThreadPoolResolvesLocalhost) {
    concurrent::ThreadPool pool("dns");
    pool.start(2);

    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    std::string ip, err;
    uint16_t port_out = 0;
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread t([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        ThreadPoolResolver resolver(&loop, &pool);
        resolver.resolve("localhost", 8080,
                         [&](const InetAddress& addr, const std::string& e) {
                             err = e;
                             ip = addr.toIp();
                             port_out = addr.port();
                             done = true;
                             loop.quit();
                         });
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 200 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(err.empty()) << "resolve error: " << err;
    // localhost resolves to a loopback address.
    EXPECT_TRUE(ip == "127.0.0.1" || ip == "::1") << "ip=" << ip;
    EXPECT_EQ(port_out, 8080);

    t.join();
    pool.stop();
}

TEST(ResolverTest, ThreadPoolResolvesNumericIp) {
    concurrent::ThreadPool pool("dns");
    pool.start(2);

    std::atomic<bool> started{false}, done{false};
    std::string ip, err;
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread t([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        ThreadPoolResolver resolver(&loop, &pool);
        resolver.resolve("127.0.0.1", 443,
                         [&](const InetAddress& addr, const std::string& e) {
                             err = e;
                             ip = addr.toIp();
                             done = true;
                             loop.quit();
                         });
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 200 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(ip, "127.0.0.1");

    t.join();
    pool.stop();
}

TEST(ResolverTest, ThreadPoolResolveAllReturnsList) {
    concurrent::ThreadPool pool("dns");
    pool.start(2);

    std::atomic<bool> started{false}, done{false};
    std::vector<InetAddress> addrs;
    std::string err;

    std::thread t([&] {
        reactor::EventLoop loop;
        ThreadPoolResolver resolver(&loop, &pool);
        resolver.resolveAll(
            "localhost", 8080,
            [&](const std::vector<InetAddress>& a, const std::string& e) {
                addrs = a;
                err = e;
                done = true;
                loop.quit();
            });
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);

    for (int i = 0; i < 200 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(err.empty()) << "resolveAll error: " << err;
    // At least one address; every entry is a loopback with the requested port.
    ASSERT_FALSE(addrs.empty());
    for (const auto& a : addrs) {
        EXPECT_TRUE(a.toIp() == "127.0.0.1" || a.toIp() == "::1")
            << "ip=" << a.toIp();
        EXPECT_EQ(a.port(), 8080);
    }

    t.join();
    pool.stop();
}

TEST(ResolverTest, ThreadPoolReportsErrorForBogusHost) {
    concurrent::ThreadPool pool("dns");
    pool.start(2);

    std::atomic<bool> started{false}, done{false};
    std::string err;
    bool had_error = false;
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread t([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        ThreadPoolResolver resolver(&loop, &pool);
        resolver.resolve("no.such.host.invalid.", 80,
                         [&](const InetAddress&, const std::string& e) {
                             had_error = !e.empty();
                             done = true;
                             loop.quit();
                         });
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 400 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(had_error);

    t.join();
    pool.stop();
}

#ifdef COX_HAVE_CARES
TEST(ResolverTest, CaresResolvesLocalhost) {
    std::atomic<bool> started{false}, done{false};
    std::string ip, err;
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread t([&] {
        reactor::EventLoop loop;
        loop_ptr = &loop;
        CaresResolver resolver(&loop);
        resolver.resolve("localhost", 8080,
                         [&](const InetAddress& addr, const std::string& e) {
                             err = e;
                             ip = addr.toIp();
                             done = true;
                             loop.quit();
                         });
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 400 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(err.empty()) << "cares error: " << err;
    EXPECT_TRUE(ip == "127.0.0.1" || ip == "::1") << "ip=" << ip;

    t.join();
}
#endif
