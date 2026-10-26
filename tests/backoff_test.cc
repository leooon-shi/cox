#include "backoff.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using flowcontrol::Backoff;

TEST(BackoffTest, ExponentialGrowth) {
    Backoff bo(100ms, 10s, /*jitter=*/0.0);  // no jitter for deterministic base
    const std::string key = "server-a";

    // Unknown key returns initial.
    EXPECT_EQ(bo.baseDelay(key), 100ms);

    bo.next(key);  // first failure -> initial
    EXPECT_EQ(bo.baseDelay(key), 100ms);

    bo.next(key);  // -> 200ms
    EXPECT_EQ(bo.baseDelay(key), 200ms);
    bo.next(key);  // -> 400ms
    EXPECT_EQ(bo.baseDelay(key), 400ms);
}

TEST(BackoffTest, CapsAtMax) {
    Backoff bo(100ms, 500ms, /*jitter=*/0.0);
    const std::string key = "k";
    for (int i = 0; i < 20; ++i) bo.next(key);

    EXPECT_EQ(bo.baseDelay(key), 500ms);  // capped
}

TEST(BackoffTest, ResetClearsState) {
    Backoff bo(100ms, 10s, 0.0);
    const std::string key = "k";
    bo.next(key);
    bo.next(key);
    EXPECT_EQ(bo.baseDelay(key), 200ms);
    bo.reset(key);
    EXPECT_EQ(bo.baseDelay(key), 100ms);  // back to initial for unknown key
}

TEST(BackoffTest, PerKeyIndependent) {
    Backoff bo(100ms, 10s, 0.0);
    bo.next("a");
    bo.next("a");
    bo.next("a");  // b -> 100ms
    bo.next("b");  // b -> 400ms

    EXPECT_EQ(bo.baseDelay("a"), 400ms);
    EXPECT_EQ(bo.baseDelay("b"), 100ms);
}

TEST(BackoffTest, JitterWithinRange) {
    Backoff bo(1000ms, 10s, /*jitter=*/0.5);
    const std::string key = "k";
    bo.next(key);  // base 1000ms
    // Jittered delay must be in [base, base*(1+jitter)) = [1000ms, 1500ms).
    for (int i = 0; i < 100; ++i) {
        auto d = bo.get(key);
        EXPECT_GE(d, 1000ms);
        EXPECT_LT(d, 1500ms);
    }
}

TEST(BackoffTest, NextDelayAdvancesAndReturns) {
    Backoff bo(100ms, 10s, 0.0);
    const std::string key = "k";

    EXPECT_EQ(bo.nextDelay(key), 100ms);  // first
    EXPECT_EQ(bo.nextDelay(key), 200ms);
    EXPECT_EQ(bo.nextDelay(key), 400ms);
}

TEST(BackoffTest, GcRemovesStaleEntries) {
    Backoff bo(100ms, 10s, 0.0);
    bo.next("old");
    std::this_thread::sleep_for(30ms);
    bo.next("fresh");

    // Remove entries older than 20ms: "old" should go, "fresh" should stay.
    bo.gc(20ms);
    // "old" is gone -> baseDelay returns initial for an unknown key.
    EXPECT_EQ(bo.baseDelay("old"), 100ms);
    // "fresh" retains its state (was advanced once -> initial 100ms; advance
    // again to prove it wasn't reset by gc).
    bo.next("fresh");
    EXPECT_EQ(bo.baseDelay("fresh"), 200ms);
}
