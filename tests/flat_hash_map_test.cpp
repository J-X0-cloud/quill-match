#include "quill/flat_hash_map.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>

namespace quill {
namespace {

TEST(FlatHashMap, StartsEmpty) {
    FlatHashMap<std::uint64_t, int> map;
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.size(), 0U);
    EXPECT_EQ(map.find(42), nullptr);
    EXPECT_FALSE(map.erase(42));
}

TEST(FlatHashMap, InsertFindErase) {
    FlatHashMap<std::uint64_t, int> map;
    EXPECT_TRUE(map.insert(7, 70));
    EXPECT_TRUE(map.insert(8, 80));
    ASSERT_NE(map.find(7), nullptr);
    EXPECT_EQ(*map.find(7), 70);
    EXPECT_EQ(*map.find(8), 80);
    EXPECT_EQ(map.size(), 2U);

    EXPECT_TRUE(map.erase(7));
    EXPECT_EQ(map.find(7), nullptr);
    EXPECT_EQ(*map.find(8), 80);
    EXPECT_EQ(map.size(), 1U);
}

TEST(FlatHashMap, DuplicateInsertKeepsOriginal) {
    FlatHashMap<std::uint64_t, int> map;
    EXPECT_TRUE(map.insert(1, 10));
    EXPECT_FALSE(map.insert(1, 99));
    EXPECT_EQ(*map.find(1), 10);
    EXPECT_EQ(map.size(), 1U);
}

TEST(FlatHashMap, EmptyKeyIsNeverFound) {
    FlatHashMap<std::uint64_t, int> map;
    map.insert(1, 1);
    EXPECT_EQ(map.find(0), nullptr);
    EXPECT_FALSE(map.erase(0));
}

TEST(FlatHashMap, ValuesAreMutableThroughFind) {
    FlatHashMap<std::uint64_t, int> map;
    map.insert(3, 1);
    *map.find(3) += 41;
    EXPECT_EQ(*map.find(3), 42);
}

TEST(FlatHashMap, GrowsAndKeepsEntries) {
    FlatHashMap<std::uint64_t, std::uint64_t> map(4);
    const std::size_t initial_capacity = map.capacity();
    for (std::uint64_t k = 1; k <= 10'000; ++k) {
        ASSERT_TRUE(map.insert(k, k * 3));
    }
    EXPECT_GT(map.capacity(), initial_capacity);
    EXPECT_LE(map.size() * 2, map.capacity());
    for (std::uint64_t k = 1; k <= 10'000; ++k) {
        ASSERT_NE(map.find(k), nullptr) << k;
        EXPECT_EQ(*map.find(k), k * 3);
    }
}

TEST(FlatHashMap, ReserveAvoidsRehash) {
    FlatHashMap<std::uint64_t, int> map;
    map.reserve(5'000);
    const std::size_t capacity = map.capacity();
    for (std::uint64_t k = 1; k <= 5'000; ++k) {
        map.insert(k, 0);
    }
    EXPECT_EQ(map.capacity(), capacity);
}

TEST(FlatHashMap, ClearRemovesEverything) {
    FlatHashMap<std::uint64_t, int> map;
    for (std::uint64_t k = 1; k <= 100; ++k) {
        map.insert(k, 1);
    }
    map.clear();
    EXPECT_TRUE(map.empty());
    for (std::uint64_t k = 1; k <= 100; ++k) {
        EXPECT_FALSE(map.contains(k));
    }
    EXPECT_TRUE(map.insert(5, 5));
}

TEST(FlatHashMap, ForEachVisitsEveryEntryOnce) {
    FlatHashMap<std::uint64_t, std::uint64_t> map;
    std::uint64_t expected_sum = 0;
    for (std::uint64_t k = 1; k <= 500; ++k) {
        map.insert(k, k);
        expected_sum += k;
    }
    std::uint64_t sum = 0;
    std::size_t count = 0;
    map.for_each([&](std::uint64_t key, std::uint64_t value) {
        EXPECT_EQ(key, value);
        sum += value;
        ++count;
    });
    EXPECT_EQ(count, 500U);
    EXPECT_EQ(sum, expected_sum);
}

// Backward-shift deletion has to repair probe chains that wrap around the end of the
// table. Using many keys in a small table forces long clusters and wrap-around.
TEST(FlatHashMap, EraseRepairsClustersUnderChurn) {
    FlatHashMap<std::uint32_t, std::uint32_t> map(8);
    std::unordered_map<std::uint32_t, std::uint32_t> reference;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<std::uint32_t> key_dist(1, 64);

    for (int step = 0; step < 200'000; ++step) {
        const std::uint32_t key = key_dist(rng);
        const auto op = rng() % 3;
        if (op == 0) {
            const bool inserted = map.insert(key, static_cast<std::uint32_t>(step));
            const bool ref_inserted =
                reference.emplace(key, static_cast<std::uint32_t>(step)).second;
            ASSERT_EQ(inserted, ref_inserted) << "step " << step;
        } else if (op == 1) {
            ASSERT_EQ(map.erase(key), reference.erase(key) == 1) << "step " << step;
        } else {
            const auto* found = map.find(key);
            const auto it = reference.find(key);
            ASSERT_EQ(found != nullptr, it != reference.end()) << "step " << step;
            if (found != nullptr) {
                ASSERT_EQ(*found, it->second);
            }
        }
        ASSERT_EQ(map.size(), reference.size());
    }
}

TEST(FlatHashMap, MatchesReferenceOnLargeRandomWorkload) {
    FlatHashMap<std::uint64_t, std::uint64_t> map;
    std::unordered_map<std::uint64_t, std::uint64_t> reference;
    std::mt19937_64 rng(99);
    for (int step = 0; step < 100'000; ++step) {
        const std::uint64_t key = (rng() % 20'000) + 1;
        if (rng() % 2 == 0) {
            map.insert(key, key ^ 0xABCDEFULL);
            reference.emplace(key, key ^ 0xABCDEFULL);
        } else {
            map.erase(key);
            reference.erase(key);
        }
    }
    ASSERT_EQ(map.size(), reference.size());
    for (const auto& [key, value] : reference) {
        ASSERT_NE(map.find(key), nullptr);
        EXPECT_EQ(*map.find(key), value);
    }
}

}  // namespace
}  // namespace quill
