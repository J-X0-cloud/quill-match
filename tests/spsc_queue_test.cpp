#include "quill/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace quill {
namespace {

TEST(SpscQueue, CapacityRoundsUpToPowerOfTwo) {
    EXPECT_EQ(SpscQueue<int>(5).capacity(), 8U);
    EXPECT_EQ(SpscQueue<int>(8).capacity(), 8U);
    EXPECT_EQ(SpscQueue<int>(0).capacity(), 2U);
}

TEST(SpscQueue, FifoOrder) {
    SpscQueue<int> q(4);
    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_EQ(q.size(), 3U);
    int v = 0;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 2);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 3);
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueue, RejectsWhenFullAndRecovers) {
    SpscQueue<int> q(4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(q.try_push(i));
    }
    EXPECT_FALSE(q.try_push(99));
    int v = 0;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_TRUE(q.try_push(4));
    for (int expected = 1; expected <= 4; ++expected) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, expected);
    }
}

TEST(SpscQueue, WrapsAroundManyTimes) {
    SpscQueue<std::uint64_t> q(8);
    std::uint64_t next_out = 0;
    std::uint64_t next_in = 0;
    for (int round = 0; round < 10'000; ++round) {
        const int burst = round % 7 + 1;
        for (int i = 0; i < burst; ++i) {
            ASSERT_TRUE(q.try_push(next_in++));
        }
        for (int i = 0; i < burst; ++i) {
            std::uint64_t v = 0;
            ASSERT_TRUE(q.try_pop(v));
            ASSERT_EQ(v, next_out++);
        }
    }
}

TEST(SpscQueue, FrontAndPopPeekWithoutCopy) {
    SpscQueue<std::string> q(4);
    EXPECT_EQ(q.front(), nullptr);
    q.try_emplace(3, 'x');
    ASSERT_NE(q.front(), nullptr);
    EXPECT_EQ(*q.front(), "xxx");
    q.pop();
    EXPECT_EQ(q.front(), nullptr);
}

TEST(SpscQueue, DestroysRemainingElements) {
    auto tracker = std::make_shared<int>(0);
    {
        SpscQueue<std::shared_ptr<int>> q(8);
        q.try_push(tracker);
        q.try_push(tracker);
        EXPECT_EQ(tracker.use_count(), 3);
    }
    EXPECT_EQ(tracker.use_count(), 1);
}

TEST(SpscQueue, PopMovesOut) {
    SpscQueue<std::unique_ptr<int>> q(2);
    q.try_emplace(std::make_unique<int>(7));
    std::unique_ptr<int> out;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 7);
}

// Two real threads hammering a small queue. Every value must arrive exactly once and in
// order; with a deliberately tiny capacity both the full and empty paths are exercised
// constantly. Running under ThreadSanitizer additionally checks the memory ordering.
TEST(SpscQueue, ConcurrentProducerConsumerPreservesSequence) {
    constexpr std::uint64_t kItems = 2'000'000;
    SpscQueue<std::uint64_t> q(64);
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            while (!q.try_push(i)) {
                cpu_relax();
            }
        }
    });
    std::uint64_t expected = 0;
    std::uint64_t checksum = 0;
    while (expected < kItems) {
        std::uint64_t v = 0;
        if (q.try_pop(v)) {
            ASSERT_EQ(v, expected);
            checksum += v;
            ++expected;
        } else {
            cpu_relax();
        }
    }
    producer.join();
    EXPECT_EQ(checksum, kItems * (kItems - 1) / 2);
    EXPECT_TRUE(q.empty());
}

}  // namespace
}  // namespace quill
