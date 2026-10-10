#include "quill/object_pool.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

namespace quill {
namespace {

struct Widget {
    std::uint64_t a = 0;
    std::uint32_t b = 0;
};

TEST(ObjectPool, AcquireConstructsValue) {
    ObjectPool<Widget> pool(8);
    Widget* w = pool.acquire(std::uint64_t{5}, std::uint32_t{6});
    EXPECT_EQ(w->a, 5U);
    EXPECT_EQ(w->b, 6U);
    EXPECT_EQ(pool.live(), 1U);
    pool.release(w);
    EXPECT_EQ(pool.live(), 0U);
}

TEST(ObjectPool, DefaultConstructsWhenNoArguments) {
    ObjectPool<Widget> pool(4);
    Widget* first = pool.acquire(std::uint64_t{9}, std::uint32_t{9});
    pool.release(first);
    Widget* second = pool.acquire();
    // The slot is reused, but the object is freshly value-initialised.
    EXPECT_EQ(second->a, 0U);
    EXPECT_EQ(second->b, 0U);
    pool.release(second);
}

TEST(ObjectPool, ReusesReleasedSlots) {
    ObjectPool<Widget> pool(4);
    Widget* w = pool.acquire();
    pool.release(w);
    EXPECT_EQ(pool.acquire(), w);
}

TEST(ObjectPool, GrowsByChunksAndKeepsPointersStable) {
    ObjectPool<Widget> pool(16);
    std::vector<Widget*> widgets;
    for (std::uint64_t i = 0; i < 100; ++i) {
        widgets.push_back(pool.acquire(i, std::uint32_t{0}));
    }
    EXPECT_EQ(pool.capacity(), 112U);  // 7 chunks of 16
    EXPECT_EQ(pool.live(), 100U);
    for (std::uint64_t i = 0; i < 100; ++i) {
        EXPECT_EQ(widgets[i]->a, i);
    }
    const std::set<Widget*> unique(widgets.begin(), widgets.end());
    EXPECT_EQ(unique.size(), widgets.size());
    for (Widget* w : widgets) {
        pool.release(w);
    }
    EXPECT_EQ(pool.live(), 0U);
}

TEST(ObjectPool, ReserveAllocatesUpFront) {
    ObjectPool<Widget> pool(10);
    pool.reserve(35);
    EXPECT_EQ(pool.capacity(), 40U);
    std::vector<Widget*> widgets;
    for (int i = 0; i < 40; ++i) {
        widgets.push_back(pool.acquire());
    }
    EXPECT_EQ(pool.capacity(), 40U);
    for (Widget* w : widgets) {
        pool.release(w);
    }
}

TEST(ObjectPool, ConsecutiveAcquisitionsAreAdjacent) {
    ObjectPool<Widget> pool(8);
    Widget* a = pool.acquire();
    Widget* b = pool.acquire();
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b) - reinterpret_cast<std::uintptr_t>(a),
              sizeof(Widget));
    pool.release(a);
    pool.release(b);
}

}  // namespace
}  // namespace quill
