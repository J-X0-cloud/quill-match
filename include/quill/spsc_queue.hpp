#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace quill {

/// Size used to keep producer-owned and consumer-owned state on separate cache lines.
/// Fixed rather than std::hardware_destructive_interference_size, whose value is not
/// ABI-stable and which GCC warns about using in headers.
inline constexpr std::size_t kCacheLine = 64;

/// Bounded lock-free single-producer/single-consumer ring buffer.
///
/// Design notes:
/// * Indices increase monotonically and are masked on access, so "full" and "empty" are
///   distinguishable without sacrificing a slot.
/// * Each side keeps a private cached copy of the other side's index and only reloads
///   the shared atomic when the cache says the queue looks full (producer) or empty
///   (consumer). In steady state this removes almost all cross-core cache traffic.
/// * The producer publishes with a release store after constructing the element; the
///   consumer acquires before reading it, and vice versa for slot reuse.
template <typename T>
class SpscQueue {
  public:
    /// `capacity` is rounded up to a power of two.
    explicit SpscQueue(std::size_t capacity)
        : capacity_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)),
          mask_(capacity_ - 1),
          slots_(std::make_unique<Slot[]>(capacity_)) {}

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    ~SpscQueue() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            while (front() != nullptr) {
                pop();
            }
        }
    }

    /// Producer side. Returns false if the queue is full.
    template <typename... Args>
    bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const std::size_t tail = producer_.tail.load(std::memory_order_relaxed);
        if (tail - producer_.cached_head == capacity_) {
            producer_.cached_head = consumer_.head.load(std::memory_order_acquire);
            if (tail - producer_.cached_head == capacity_) {
                return false;
            }
        }
        ::new (static_cast<void*>(slots_[tail & mask_].bytes)) T(std::forward<Args>(args)...);
        producer_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool try_push(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return try_emplace(value);
    }

    /// Consumer side. Moves the oldest element into `out`; returns false if empty.
    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t head = consumer_.head.load(std::memory_order_relaxed);
        if (head == consumer_.cached_tail) {
            consumer_.cached_tail = producer_.tail.load(std::memory_order_acquire);
            if (head == consumer_.cached_tail) {
                return false;
            }
        }
        T* element = std::launder(reinterpret_cast<T*>(slots_[head & mask_].bytes));
        out = std::move(*element);
        element->~T();
        consumer_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns a pointer to the oldest element without removing it.
    [[nodiscard]] T* front() noexcept {
        const std::size_t head = consumer_.head.load(std::memory_order_relaxed);
        if (head == consumer_.cached_tail) {
            consumer_.cached_tail = producer_.tail.load(std::memory_order_acquire);
            if (head == consumer_.cached_tail) {
                return nullptr;
            }
        }
        return std::launder(reinterpret_cast<T*>(slots_[head & mask_].bytes));
    }

    /// Consumer side. Discards the element returned by front().
    void pop() noexcept {
        const std::size_t head = consumer_.head.load(std::memory_order_relaxed);
        assert(head != producer_.tail.load(std::memory_order_acquire));
        std::launder(reinterpret_cast<T*>(slots_[head & mask_].bytes))->~T();
        consumer_.head.store(head + 1, std::memory_order_release);
    }

    /// Approximate when called concurrently with either side.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t tail = producer_.tail.load(std::memory_order_acquire);
        const std::size_t head = consumer_.head.load(std::memory_order_acquire);
        return tail - head;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  private:
    struct Slot {
        alignas(T) std::byte bytes[sizeof(T)];
    };

    struct alignas(kCacheLine) ProducerState {
        std::atomic<std::size_t> tail{0};
        std::size_t cached_head = 0;
    };

    struct alignas(kCacheLine) ConsumerState {
        std::atomic<std::size_t> head{0};
        std::size_t cached_tail = 0;
    };

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<Slot[]> slots_;
    ProducerState producer_;
    ConsumerState consumer_;
};

/// Busy-wait hint for spin loops.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

}  // namespace quill
