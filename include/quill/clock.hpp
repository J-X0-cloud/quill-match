#pragma once

#include <chrono>

#include "quill/types.hpp"

namespace quill {

/// Time source for event timestamps. Injected so tests can be fully deterministic.
class Clock {
  public:
    Clock() = default;
    Clock(const Clock&) = default;
    Clock& operator=(const Clock&) = default;
    Clock(Clock&&) = default;
    Clock& operator=(Clock&&) = default;
    virtual ~Clock() = default;

    [[nodiscard]] virtual Timestamp now() noexcept = 0;
};

/// Monotonic wall time in nanoseconds.
class SteadyClock final : public Clock {
  public:
    [[nodiscard]] Timestamp now() noexcept override { return read(); }

    [[nodiscard]] static Timestamp read() noexcept {
        return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
    }
};

/// A clock that only moves when told to. Each read advances it by `step` so that events
/// within one command still get distinct, increasing timestamps.
class ManualClock final : public Clock {
  public:
    explicit ManualClock(Timestamp start = 0, Timestamp step = 1) noexcept
        : now_(start), step_(step) {}

    [[nodiscard]] Timestamp now() noexcept override {
        const Timestamp t = now_;
        now_ += step_;
        return t;
    }

    void set(Timestamp t) noexcept { now_ = t; }
    void advance(Timestamp delta) noexcept { now_ += delta; }

  private:
    Timestamp now_;
    Timestamp step_;
};

}  // namespace quill
