#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <ostream>

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "quill/itch/book_builder.hpp"
#include "quill/itch/codec.hpp"

namespace quill::cli {

namespace {

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw UsageError("cannot open " + path);
    }
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(size);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!file) {
        throw UsageError("failed to read " + path);
    }
    return bytes;
}

}  // namespace

ReplayResult replay(const std::string& path, std::size_t depth,
                    const std::optional<std::string>& symbol, std::ostream& out) {
    ReplayResult result;
    const std::vector<std::uint8_t> bytes = read_file(path);
    result.bytes = bytes.size();

    itch::BookBuilder builder;
    const auto start = std::chrono::steady_clock::now();
    const itch::DecodeResult decoded = itch::decode(bytes, builder);
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    result.messages = decoded.messages;
    result.skipped = decoded.skipped;
    if (decoded.error) {
        result.ok = false;
        result.error = std::string(itch::to_string(*decoded.error)) + " at byte " +
                       std::to_string(decoded.consumed);
    } else if (decoded.consumed != bytes.size()) {
        result.ok = false;
        result.error = "truncated final frame at byte " + std::to_string(decoded.consumed);
    }

    const auto& s = builder.stats();
    out << "replayed " << with_commas(result.messages) << " messages (" << with_commas(result.bytes)
        << " bytes) in " << std::fixed << std::setprecision(2) << result.seconds * 1e3 << " ms, "
        << rate(static_cast<double>(result.messages) / std::max(result.seconds, 1e-9)) << '\n';
    out.unsetf(std::ios::fixed);
    out << "  adds " << with_commas(s.adds) << ", executions " << with_commas(s.executions)
        << ", cancels " << with_commas(s.cancels) << ", deletes " << with_commas(s.deletes)
        << ", replaces " << with_commas(s.replaces) << '\n'
        << "  directory " << s.directory << ", system events " << s.system_events << ", skipped "
        << result.skipped << ", anomalies " << s.anomalies << '\n'
        << "  live orders at end " << with_commas(builder.live_orders()) << '\n';
    if (!result.ok) {
        out << "  stopped early: " << result.error << '\n';
    }

    std::vector<std::uint16_t> locates = builder.locates();
    if (symbol) {
        const auto found = builder.find(*symbol);
        if (!found) {
            throw UsageError("symbol " + *symbol + " not present in the feed");
        }
        locates = {*found};
    }
    for (const std::uint16_t locate : locates) {
        const auto* state = builder.instrument(locate);
        out << '\n'
            << state->symbol << " (locate " << locate << "): " << with_commas(state->executions)
            << " executions, " << with_commas(static_cast<std::uint64_t>(state->executed_volume))
            << " units traded\n";
        print_ladder(out, builder.depth(locate, Side::Buy, depth),
                     builder.depth(locate, Side::Sell, depth));
    }
    return result;
}

int replay_command(const std::vector<std::string>& args, std::ostream& out) {
    const Args parsed(args, {"depth", "symbol"}, {});
    if (parsed.positionals().size() != 1) {
        throw UsageError("replay needs exactly one ITCH file");
    }
    const auto depth = static_cast<std::size_t>(parsed.integer("depth", 5, 1, 1'000));
    const ReplayResult result = replay(parsed.positionals()[0], depth, parsed.value("symbol"), out);
    return result.ok ? 0 : 2;
}

}  // namespace quill::cli
