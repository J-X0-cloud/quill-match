#include <chrono>
#include <istream>
#include <ostream>
#include <string>

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "quill/clock.hpp"
#include "quill/fix/gateway.hpp"
#include "quill/matching_engine.hpp"

namespace quill::cli {

namespace {

/// Wall-clock time, so SendingTime fields carry the real UTC time.
class SystemClock final : public Clock {
  public:
    [[nodiscard]] Timestamp now() noexcept override {
        return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    }
};

}  // namespace

std::uint64_t run_fix_console(const FixConsoleOptions& options, std::istream& in,
                              std::ostream& out) {
    SystemClock clock;
    EventFanout fanout;
    MatchingEngine engine(fanout, clock);
    fix::GatewayConfig config;
    config.delimiter = options.delimiter;
    config.strict = options.strict;
    fix::Gateway gateway(engine, clock, config);
    fanout.add(gateway);
    for (const std::string& symbol : options.symbols) {
        InstrumentSpec spec;
        spec.symbol = symbol;
        engine.add_instrument(spec);
    }

    std::uint64_t processed = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;  // allow commented scripts
        }
        ++processed;
        for (const fix::Outbound& message : gateway.handle(line)) {
            out << "-> " << (message.session.empty() ? "?" : message.session) << ' ' << message.text
                << '\n';
        }
    }
    return processed;
}

int fix_command(const std::vector<std::string>& args, std::istream& in, std::ostream& out) {
    const Args parsed(args, {"delimiter", "symbols"}, {"strict"});
    if (!parsed.positionals().empty()) {
        throw UsageError("fix reads messages from standard input and takes no file argument");
    }
    FixConsoleOptions options;
    const std::string delimiter = parsed.value_or("delimiter", "pipe");
    if (delimiter == "pipe") {
        options.delimiter = '|';
    } else if (delimiter == "soh") {
        options.delimiter = fix::kSoh;
    } else {
        throw UsageError("--delimiter must be 'pipe' or 'soh'");
    }
    options.strict = parsed.flag("strict");
    if (const auto symbols = parsed.value("symbols")) {
        options.symbols = split_list(*symbols);
        if (options.symbols.empty()) {
            throw UsageError("--symbols needs at least one symbol");
        }
    }
    run_fix_console(options, in, out);
    return 0;
}

}  // namespace quill::cli
