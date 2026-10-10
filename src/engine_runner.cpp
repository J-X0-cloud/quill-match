#include "quill/engine_runner.hpp"

#include <stdexcept>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "quill/clock.hpp"

namespace quill {

namespace {

void pin_current_thread(int cpu) {
#if defined(__linux__)
    if (cpu < 0) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    // Best effort: pinning can legitimately fail in containers with restricted cpusets.
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

}  // namespace

EngineRunner::EngineRunner(MatchingEngine& engine, std::size_t queue_capacity)
    : engine_(&engine), queue_(queue_capacity) {}

EngineRunner::~EngineRunner() {
    stop();
}

void EngineRunner::start(int cpu) {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        throw std::logic_error("engine runner already started");
    }
    stop_requested_.store(false, std::memory_order_release);
    thread_ = std::thread([this, cpu] {
        pin_current_thread(cpu);
        run();
    });
}

void EngineRunner::stop() {
    if (!thread_.joinable()) {
        return;
    }
    stop_requested_.store(true, std::memory_order_release);
    thread_.join();
    running_.store(false, std::memory_order_release);
}

bool EngineRunner::try_submit(Command command) noexcept {
    command.enqueued_at = SteadyClock::read();
    return queue_.try_push(command);
}

std::uint64_t EngineRunner::submit(Command command) noexcept {
    command.enqueued_at = SteadyClock::read();
    std::uint64_t spins = 0;
    while (!queue_.try_push(command)) {
        ++spins;
        cpu_relax();
    }
    return spins;
}

void EngineRunner::run() {
    std::uint64_t processed = 0;
    for (;;) {
        Command* command = queue_.front();
        if (command == nullptr) {
            // Only exit once a stop was requested *and* the queue is observed empty after
            // that request, so nothing submitted before stop() is lost.
            if (stop_requested_.load(std::memory_order_acquire) && queue_.front() == nullptr) {
                break;
            }
            cpu_relax();
            continue;
        }
        engine_->process(*command);
        const Timestamp done = SteadyClock::read();
        latency_.record(done >= command->enqueued_at ? done - command->enqueued_at : 0);
        queue_.pop();
        processed_.store(++processed, std::memory_order_release);
    }
}

}  // namespace quill
