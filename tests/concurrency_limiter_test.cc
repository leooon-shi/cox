#include "concurrency_limiter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using flowcontrol::ConcurrencyLimiter;

TEST(ConcurrencyLimiterTest, AdmitsUpToMax)
{
    ConcurrencyLimiter limiter(3);
    auto p1 = limiter.tryAcquire();
    auto p2 = limiter.tryAcquire();
    auto p3 = limiter.tryAcquire();

    EXPECT_TRUE(p1);
    EXPECT_TRUE(p2);
    EXPECT_TRUE(p3);
    EXPECT_EQ(limiter.inFlight(), 3u);

    auto p4 = limiter.tryAcquire();  // over capacity
    EXPECT_FALSE(p4);
    EXPECT_EQ(limiter.inFlight(), 3u);
    EXPECT_TRUE(limiter.isOverloaded());
}

TEST(ConcurrencyLimiterTest, ReleaseFreeSlot)
{
    ConcurrencyLimiter limiter(1);
    {
        auto p = limiter.tryAcquire();
        EXPECT_TRUE(p);
        EXPECT_FALSE(limiter.tryAcquire());  // full
    }  // p released here

    EXPECT_EQ(limiter.inFlight(), 0u);
    auto p2 = limiter.tryAcquire();  // Slot available again.
    EXPECT_TRUE(p2);
}

TEST(ConcurrencyLimiterTest, ExplicitRelease)
{
    ConcurrencyLimiter limiter(1);
    auto p = limiter.tryAcquire();
    EXPECT_TRUE(p);
    p.release();
    EXPECT_EQ(limiter.inFlight(), 0u);
    p.release();  // idempotent
    EXPECT_EQ(limiter.inFlight(), 0u);
}

TEST(ConcurrencyLimiterTest, MoveTransfersOwnership)
{
    ConcurrencyLimiter limiter(1);
    auto p = limiter.tryAcquire();
    EXPECT_EQ(limiter.inFlight(), 1u);
    auto p2 = std::move(p);
    EXPECT_FALSE(p);    // moved-from is empty
    EXPECT_TRUE(p2);
    EXPECT_EQ(limiter.inFlight(), 1u);   // Still one slot held
}

TEST(ConcurrencyLimiterTest, UnlimitedAlwaysAdmits)
{
    ConcurrencyLimiter limiter(0);   // 0 = unlimited
    std::vector<ConcurrencyLimiter::Permit> permits;
    for (int i = 0; i < 1000, ++i)
    {
        auto p = limiter.tryAcquire();
        EXPECT_TRUE(p);
        permits.push_back(std::move(p));
    }
    EXPECT_EQ(limiter.inFlight(), 1000u);
    EXPECT_FALSE(limiter.isOverloaded());    // never overloaded when unlimited
}

TEST(ConcurrencyLimiterTest, ConcurrentNeverExceedsMax)
{
    constexpr size_t kMax = 8;
    ConcurrencyLimiter limiter(kMax);
    std::atomic<size_t> peak{0};
    std::atomic<int> admitted{0};

    auto worker = [&] {
        for (int i = 0; i < 2000; ++i)
        {
            auto p = limiter.tryAcquire();
            if (p)
            {
                admitted++;
                size_t cur = limiter.inFlight();
                size_t prev = peak.load();
                while (cur > prev && !peak.compare_exchange_weak(prev, cur)) {}
                std::this_thread::yield();
                // p releases at end of iteration
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    EXPECT_LE(peak.load(), kMax);   // invariant: never exceed max concurrency
    EXPECT_GT(admitted.load(), 0);

    EXPECT_EQ(limiter.inFlight(), 0u);    // all released
}