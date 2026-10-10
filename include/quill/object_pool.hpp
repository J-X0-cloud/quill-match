#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace quill {

/// Fixed-type slab allocator.
///
/// Objects are carved out of chunks that are allocated once and never returned to the
/// system while the pool lives, so steady-state allocation is a pop from a free list and
/// release is a push. Because chunks are never moved, pointers handed out stay valid until
/// they are released. The pool requires trivially destructible types so that it can be
/// torn down without tracking which slots are live.
template <typename T>
class ObjectPool {
    static_assert(std::is_trivially_destructible_v<T>,
                  "ObjectPool only manages trivially destructible types");

  public:
    explicit ObjectPool(std::size_t chunk_size = 4096) : chunk_size_(chunk_size) {
        assert(chunk_size_ > 0);
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) noexcept = default;
    ObjectPool& operator=(ObjectPool&&) noexcept = default;
    ~ObjectPool() = default;

    /// Constructs a T in a free slot and returns a pointer to it.
    template <typename... Args>
    [[nodiscard]] T* acquire(Args&&... args) {
        if (free_.empty()) {
            grow();
        }
        Slot* slot = free_.back();
        free_.pop_back();
        ++live_;
        return ::new (static_cast<void*>(slot->bytes)) T{std::forward<Args>(args)...};
    }

    /// Returns a slot to the pool. The pointer must have come from this pool.
    void release(T* object) noexcept {
        assert(object != nullptr);
        assert(live_ > 0);
        --live_;
        // Trivially destructible, so ending the lifetime needs no destructor call. The
        // free list never allocates here because its capacity always covers every slot.
        free_.push_back(reinterpret_cast<Slot*>(object));
    }

    /// Pre-allocates enough chunks to hold at least `count` live objects.
    void reserve(std::size_t count) {
        while (capacity() < count) {
            grow();
        }
    }

    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return chunks_.size() * chunk_size_; }

  private:
    struct Slot {
        alignas(T) std::byte bytes[sizeof(T)];
    };

    void grow() {
        auto chunk = std::make_unique<Slot[]>(chunk_size_);
        Slot* base = chunk.get();
        chunks_.push_back(std::move(chunk));
        free_.reserve(capacity());
        // Push in reverse so that acquisition walks the chunk front to back, which keeps
        // consecutive allocations adjacent in memory.
        for (std::size_t i = chunk_size_; i > 0; --i) {
            free_.push_back(base + (i - 1));
        }
    }

    std::size_t chunk_size_;
    std::size_t live_ = 0;
    std::vector<std::unique_ptr<Slot[]>> chunks_;
    std::vector<Slot*> free_;
};

}  // namespace quill
