#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <stdexcept>

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "quill/clock.hpp"
#include "quill/engine_runner.hpp"
#include "quill/itch/publisher.hpp"
#include "quill/matching_engine.hpp"
#include "quill/sim/order_flow.hpp"

namespace quill::cli {

namespace {

/// Smallest observable difference between two back-to-back clock reads: the floor below
/// which no latency figure from this tool is meaningful.
std::uint64_t measure_timer_overhead() {
    std::uint64_t best = UINT64_MAX;
    for (int i = 0; i < 10'000; ++i) {
        const Timestamp a = SteadyClock::read();
        const Timestamp b = SteadyClock::read();
        best = std::min<std::uint64_t>(best, b - a);
    }
    return best;
}

std::string instrument_symbol(std::uint32_t index) {
    std::string symbol = "SYM" + std::to_string(index);
    return symbol;
}

}  // namespace

SimulationResult simulate(const SimulationOptions& options, std::ostream& out) {
    SimulationResult result;
    itch::Publisher publisher;
    EventFanout fanout;
    fanout.add(result.stats);
    if (options.itch_out) {
        fanout.add(publisher);
    }
    SteadyClock clock;
    MatchingEngine engine(fanout, clock);

    std::vector<InstrumentId> ids;
    if (options.itch_out) {
        publisher.start_of_messages(clock.now());
    }
    for (std::uint32_t i = 1; i <= options.instruments; ++i) {
        InstrumentSpec spec;
        spec.symbol = instrument_symbol(i);
        const InstrumentId id = engine.add_instrument(spec);
        ids.push_back(id);
        if (options.itch_out) {
            publisher.register_instrument(id, spec, clock.now());
        }
    }

    sim::FlowConfig config;
    config.instruments = ids;
    config.seed = options.seed;
    sim::OrderFlowGenerator generator(config);
    out << "generating " << with_commas(options.warmup + options.orders) << " commands across "
        << options.instruments << " instruments (seed " << options.seed << ")\n";
    const std::vector<Command> commands = generator.generate(options.warmup + options.orders);
    result.timer_overhead_ns = measure_timer_overhead();

    for (std::uint64_t i = 0; i < options.warmup; ++i) {
        engine.process(commands[i]);
    }
    result.commands = options.orders;

    const auto start = std::chrono::steady_clock::now();
    if (options.threaded) {
        EngineRunner runner(engine, options.queue_capacity);
        runner.start(options.pin_cpu);
        const Timestamp first_send = SteadyClock::read();
        const double interval_ns = options.rate > 0 ? 1e9 / static_cast<double>(options.rate) : 0;
        for (std::uint64_t i = options.warmup; i < commands.size(); ++i) {
            if (options.rate > 0) {
                const auto due =
                    first_send +
                    static_cast<Timestamp>(static_cast<double>(i - options.warmup) * interval_ns);
                while (SteadyClock::read() < due) {
                    cpu_relax();
                }
            }
            result.back_pressure_spins += runner.submit(commands[i]);
        }
        runner.stop();
        result.latency = runner.latency();
    } else {
        for (std::uint64_t i = options.warmup; i < commands.size(); ++i) {
            const Timestamp t0 = SteadyClock::read();
            engine.process(commands[i]);
            const Timestamp t1 = SteadyClock::read();
            result.latency.record(t1 - t0);
        }
    }
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (options.itch_out) {
        publisher.end_of_messages(clock.now());
        result.itch_messages = publisher.messages();
        result.itch_bytes = publisher.writer().size();
        std::ofstream file(*options.itch_out, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("cannot open " + *options.itch_out + " for writing");
        }
        publisher.flush(file);
    }

    for (const InstrumentId id : ids) {
        if (const auto problem = engine.book(id)->check_invariants()) {
            throw std::logic_error("book invariant violated after simulation: " + *problem);
        }
    }
    return result;
}

int simulate_command(const std::vector<std::string>& args, std::ostream& out) {
    const Args parsed(args,
                      {"orders", "instruments", "seed", "warmup", "queue-capacity", "pin-cpu",
                       "itch-out", "rate"},
                      {"threaded"});
    if (!parsed.positionals().empty()) {
        throw UsageError("simulate takes no positional arguments");
    }
    SimulationOptions options;
    options.orders =
        static_cast<std::uint64_t>(parsed.integer("orders", 1'000'000, 1, 500'000'000));
    options.instruments = static_cast<std::uint32_t>(parsed.integer("instruments", 4, 1, 1'000));
    options.seed = static_cast<std::uint64_t>(parsed.integer("seed", 1, 0, INT64_MAX));
    options.warmup = static_cast<std::uint64_t>(parsed.integer("warmup", 100'000, 0, 100'000'000));
    options.threaded = parsed.flag("threaded");
    options.queue_capacity =
        static_cast<std::size_t>(parsed.integer("queue-capacity", 1 << 16, 2, 1 << 26));
    options.pin_cpu = static_cast<int>(parsed.integer("pin-cpu", -1, -1, 4'095));
    options.itch_out = parsed.value("itch-out");
    options.rate = static_cast<std::uint64_t>(parsed.integer("rate", 0, 0, 1'000'000'000));
    if (options.rate > 0 && !options.threaded) {
        throw UsageError("--rate paces the producer thread and requires --threaded");
    }

    const SimulationResult result = simulate(options, out);
    out << '\n'
        << (options.threaded ? "threaded: producer -> SPSC queue -> matching thread\n"
                             : "synchronous: matching on the calling thread\n")
        << "  commands   " << with_commas(result.commands) << " in " << std::fixed
        << std::setprecision(3) << result.seconds << " s ("
        << rate(static_cast<double>(result.commands) / result.seconds) << ")\n";
    out.unsetf(std::ios::fixed);
    if (options.threaded) {
        out << "  producer "
            << (options.rate > 0 ? "paced at " + rate(static_cast<double>(options.rate))
                                 : std::string("unpaced (queue saturates)"))
            << ", back-pressure spins " << with_commas(result.back_pressure_spins) << '\n';
    }
    out << "\nengine totals (including " << with_commas(options.warmup) << " warm-up commands)\n";
    result.stats.print(out);
    out << '\n'
        << (options.threaded ? "latency: enqueue to matched, per command\n"
                             : "latency: per command, matching only\n");
    result.latency.print_percentiles(out);
    out << "  (timer resolution floor about " << result.timer_overhead_ns << " ns)\n";
    if (options.itch_out) {
        out << "\nwrote " << with_commas(result.itch_messages) << " ITCH messages ("
            << with_commas(result.itch_bytes) << " bytes) to " << *options.itch_out
            << "; latency above includes feed encoding\n";
    }
    return 0;
}

}  // namespace quill::cli
