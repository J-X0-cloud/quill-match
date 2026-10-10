#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace quill {

/// Finalizer from SplitMix64. Spreads sequential keys (the common case for order ids)
/// across the whole table so that linear probing does not form long runs.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    x ^= x >> 30U;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31U;
    return x;
}

/// Open-addressing hash map for unsigned integer keys.
///
/// * Power-of-two capacity, linear probing, maximum load factor of 1/2.
/// * Deletion uses backward-shift instead of tombstones, so lookups never degrade after
///   heavy insert/erase churn (which is exactly the order-book workload).
/// * One key value is reserved to mark empty slots and may not be inserted.
template <typename Key, typename Value, Key EmptyKey = Key{0}>
class FlatHashMap {
    static_assert(std::is_unsigned_v<Key>, "FlatHashMap expects unsigned integer keys");
    static_assert(std::is_trivially_copyable_v<Value>,
                  "FlatHashMap moves values with plain copies during backward shift");

public:
    explicit FlatHashMap(std::size_t expected = 16) { rehash(capacity_for(expected)); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    /// Returns a pointer to the value for `key`, or nullptr when absent.
    [[nodiscard]] Value* find(Key key) noexcept {
        if (key == EmptyKey) {
            return nullptr;
        }
        for (std::size_t i = home(key);; i = next(i)) {
            Slot& slot = slots_[i];
            if (slot.key == key) {
                return &slot.value;
            }
            if (slot.key == EmptyKey) {
                return nullptr;
            }
        }
    }

    [[nodiscard]] const Value* find(Key key) const noexcept {
        return const_cast<FlatHashMap*>(this)->find(key);
    }

    [[nodiscard]] bool contains(Key key) const noexcept { return find(key) != nullptr; }

    /// Inserts `key -> value` if `key` is absent. Returns false (and leaves the existing
    /// mapping untouched) if it was already present.
    bool insert(Key key, const Value& value) {
        assert(key != EmptyKey);
        if ((size_ + 1) * 2 > slots_.size()) {
            rehash(slots_.size() * 2);
        }
        for (std::size_t i = home(key);; i = next(i)) {
            Slot& slot = slots_[i];
            if (slot.key == key) {
                return false;
            }
            if (slot.key == EmptyKey) {
                slot.key = key;
                slot.value = value;
                ++size_;
                return true;
            }
        }
    }

    /// Removes `key`. Returns false if it was not present.
    bool erase(Key key) noexcept {
        if (key == EmptyKey) {
            return false;
        }
        std::size_t hole = home(key);
        while (slots_[hole].key != key) {
            if (slots_[hole].key == EmptyKey) {
                return false;
            }
            hole = next(hole);
        }
        // Backward-shift: walk the cluster after the hole and pull back any entry whose
        // home position lies cyclically at or before the hole.
        for (std::size_t probe = next(hole);; probe = next(probe)) {
            Slot& candidate = slots_[probe];
            if (candidate.key == EmptyKey) {
                break;
            }
            const std::size_t ideal = home(candidate.key);
            if (distance(ideal, probe) >= distance(hole, probe)) {
                slots_[hole] = candidate;
                hole = probe;
            }
        }
        slots_[hole].key = EmptyKey;
        --size_;
        return true;
    }

    void clear() noexcept {
        for (Slot& slot : slots_) {
            slot.key = EmptyKey;
        }
        size_ = 0;
    }

    void reserve(std::size_t expected) {
        const std::size_t wanted = capacity_for(expected);
        if (wanted > slots_.size()) {
            rehash(wanted);
        }
    }

    /// Invokes `fn(key, value)` for every entry, in unspecified order.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const Slot& slot : slots_) {
            if (slot.key != EmptyKey) {
                fn(slot.key, slot.value);
            }
        }
    }

private:
    struct Slot {
        Key key = EmptyKey;
        Value value{};
    };

    static std::size_t capacity_for(std::size_t expected) noexcept {
        return std::bit_ceil(std::max<std::size_t>(expected * 2, 16));
    }

    [[nodiscard]] std::size_t home(Key key) const noexcept {
        return static_cast<std::size_t>(mix64(static_cast<std::uint64_t>(key))) & mask_;
    }
    [[nodiscard]] std::size_t next(std::size_t index) const noexcept {
        return (index + 1) & mask_;
    }
    /// Probe distance travelled from `from` to reach `to`, accounting for wrap-around.
    [[nodiscard]] std::size_t distance(std::size_t from, std::size_t to) const noexcept {
        return (to - from) & mask_;
    }

    void rehash(std::size_t new_capacity) {
        assert(std::has_single_bit(new_capacity));
        std::vector<Slot> old = std::exchange(slots_, std::vector<Slot>(new_capacity));
        mask_ = new_capacity - 1;
        size_ = 0;
        for (const Slot& slot : old) {
            if (slot.key != EmptyKey) {
                for (std::size_t i = home(slot.key);; i = next(i)) {
                    if (slots_[i].key == EmptyKey) {
                        slots_[i] = slot;
                        ++size_;
                        break;
                    }
                }
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

}  // namespace quill
