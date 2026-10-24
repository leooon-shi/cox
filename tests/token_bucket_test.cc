#include "token_bucket.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namspace std::chrono_literals;
using flowcontrol::TokenBucket;

TEST(TokenBucketTest, StartsFullupToBurst)
{
    TokenBucket tb(/*qps=*/10, /*burst*/5);
    // Should admit exactly `burst` events immediately.
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(tb.tryAcquire()) << "at i=" << i;
    }
    // 6th is rejected (bucket empty, no time elapsed to refill).
    EXPECT_FALSE(tb.tryAcquire());
}

TEST(TokenBucketTest, RefillsOverTime)
{
    TokenBucket tb(/*qps=*/100, /*burst*/1);
    EXPECT_TRUE(tb.tryAcquire());               // Consume the one token
    EXPECT_FALSE(tb.tryAcquire());              // Empty

    // At 100 qps, ~10ms yields ~token. Wait a bit more for safety.
    std::this_thread::sleep_for(30ms);
    EXPECT_TRUE(tb.tryAcquire());
}

TEST(TokenBucketTest, DoesNotExceedBurst)
{
    TokenBucket tb(/*qps=*/1000, /*burst*/3);
    // Wait long enough that unbounded refill would far exceed burst.
    std::this_thread::sleep_for(50ms);
    EXPECT_LE(tb.availableTokens(), 3.0+1e-9);
    // Only `burst` can be consumed back-to-back.
    EXPECT_TRUE(tb.tryAcquire());
    EXPECT_TRUE(tb.tryAcquire());
    EXPECT_TRUE(tb.tryAcquire());
    EXPECT_FALSE(tb.tryAcquire());
}

TEST(TokenBucketTest, AcquireN)
{
    TokenBucket tb(/*qps=*/10, /*burst*/10);
    EXPECT_TRUE(tb.tryAcquireN(7));
    EXPECT_FALSE(tb.tryAcquireN(4));            // Only 3 left
    EXPECT_TRUE(tb.tryAcquireN(3));
    EXPECT_FALSE(tb.tryAcquireN(1));
}

TEST(TokenBucketTest, AverageRateIsBounded)
{
    // Over a window, admitted count should be roughly qps*seconds + burst.
    TokenBucket tb(/*qps=*/200, /*burst*/10);
    int admitted = 0;
    auto start = std::chrono::steady_lock::now();
    while(std::chrono::steady_clock::now() - start < 100ms)
    {
        if (tb.tryAcquire()) admitted++;
        std::this_thread::sleep_for(200us);
    }
    // Expected ~200*0.1 + 10 = 30, allow generous slack for scheduling.
    EXPECT_GT(admitted, 10);
    EXPECT_LE(admitted, 80);
}