#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "quill/commands.hpp"
#include "quill/latency_histogram.hpp"
#include "quill/matching_engine.hpp"
#include "quill/spsc_queue.hpp"

namespace quill {

/// Runs a MatchingEngine on a dedicated thread fed by a lock-free SPSC ingress queue.
///
/// Exactly one producer thread may call submit()/try_submit(). The engine, and every
/// listener attached to it, is only touched by the matching thread while the runner is
/// started, so they need no synchronisation of their own. For each command the runner
/// records the time from `Command::enqueued_at` to the end of matching, i.e. queueing
/// delay plus processing time, in a histogram that is safe to read after stop().
class EngineRunner {
  public:
    EngineRunner(MatchingEngine& engine, std::size_t queue_capacity);

    EngineRunner(const EngineRunner&) = delete;
    EngineRunner& operator=(const EngineRunner&) = delete;
    EngineRunner(EngineRunner&&) = delete;
    EngineRunner& operator=(EngineRunner&&) = delete;
    ~EngineRunner();

    /// Starts the matching thread. Optionally pins it to `cpu` (Linux only; ignored if
    /// negative or unsupported).
    void start(int cpu = -1);

    /// Signals the matching thread to finish everything already queued, then joins it.
    void stop();

    /// Stamps the command and enqueues it; returns false if the queue is full.
    bool try_submit(Command command) noexcept;

    /// Spins until the command is enqueued. Returns the number of failed attempts, which
    /// is a direct measure of back-pressure.
    std::uint64_t submit(Command command) noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t processed() const noexcept {
        return processed_.load(std::memory_order_acquire);
    }

    /// Queue-to-matched latency. Only read this after stop().
    [[nodiscard]] const LatencyHistogram& latency() const noexcept { return latency_; }

  private:
    void run();

    MatchingEngine* engine_;
    SpscQueue<Command> queue_;
    LatencyHistogram latency_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> processed_{0};
};

}  // namespace quill
