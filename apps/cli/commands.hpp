#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "cli/report.hpp"
#include "quill/latency_histogram.hpp"

namespace quill::cli {

struct SimulationOptions {
    std::uint64_t orders = 1'000'000;
    std::uint32_t instruments = 4;
    std::uint64_t seed = 1;
    std::uint64_t warmup = 100'000;
    bool threaded = false;
    /// Producer pacing in commands per second for threaded runs; zero means flat out.
    /// Unpaced runs saturate the queue, so their latency is mostly time spent queued.
    std::uint64_t rate = 0;
    std::size_t queue_capacity = 1 << 16;
    int pin_cpu = -1;
    std::optional<std::string> itch_out;
};

struct SimulationResult {
    LatencyHistogram latency;
    StatsListener stats;
    double seconds = 0;
    std::uint64_t commands = 0;
    std::uint64_t back_pressure_spins = 0;
    std::uint64_t itch_messages = 0;
    std::uint64_t itch_bytes = 0;
    std::uint64_t timer_overhead_ns = 0;
};

/// Runs synthetic flow through the engine and measures it. Generation happens before the
/// clock starts, so only matching (and, when threaded, queueing) is timed.
[[nodiscard]] SimulationResult simulate(const SimulationOptions& options, std::ostream& out);

struct ReplayResult {
    std::uint64_t bytes = 0;
    std::uint64_t messages = 0;
    std::uint64_t skipped = 0;
    double seconds = 0;
    bool ok = true;
    std::string error;
};

/// Decodes an ITCH file and rebuilds every book, printing statistics and depth.
[[nodiscard]] ReplayResult replay(const std::string& path, std::size_t depth,
                                  const std::optional<std::string>& symbol, std::ostream& out);

struct FixConsoleOptions {
    char delimiter = '|';
    bool strict = false;
    std::vector<std::string> symbols{"AAPL", "MSFT", "GOOG"};
};

/// Reads one FIX message per line from `in` and writes every outbound message to `out`.
/// Returns the number of inbound messages processed.
std::uint64_t run_fix_console(const FixConsoleOptions& options, std::istream& in,
                              std::ostream& out);

/// Subcommand entry points: parse arguments, run, print. Return the process exit code.
int simulate_command(const std::vector<std::string>& args, std::ostream& out);
int replay_command(const std::vector<std::string>& args, std::ostream& out);
int fix_command(const std::vector<std::string>& args, std::istream& in, std::ostream& out);

}  // namespace quill::cli
