#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "cli/args.hpp"
#include "cli/commands.hpp"

namespace {

constexpr std::string_view kUsage = R"(usage: quill <command> [options]

commands:
  simulate   drive the engine with synthetic order flow and report latency
             --orders N          measured commands (default 1000000)
             --warmup N          unmeasured commands first (default 100000)
             --instruments N     number of instruments (default 4)
             --seed N            order flow seed (default 1)
             --threaded          feed a matching thread through the SPSC queue
             --rate N            pace the producer at N commands/s (--threaded only)
             --queue-capacity N  ring size for --threaded (default 65536)
             --pin-cpu N         pin the matching thread (Linux, --threaded only)
             --itch-out FILE     also publish the ITCH feed to FILE

  replay     rebuild books from an ITCH file and print depth
             replay FILE [--depth N] [--symbol SYM]

  fix        run a FIX order-entry session over stdin/stdout, one message per line
             --delimiter pipe|soh  field delimiter (default pipe)
             --strict              require valid BodyLength and CheckSum
             --symbols A,B,C       instruments to list (default AAPL,MSFT,GOOG)

  help       show this text
)";

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> all(argv + 1, argv + argc);
    if (all.empty() || all[0] == "help" || all[0] == "--help" || all[0] == "-h") {
        std::cout << kUsage;
        return all.empty() ? 1 : 0;
    }
    const std::string command = all[0];
    const std::vector<std::string> rest(all.begin() + 1, all.end());
    try {
        if (command == "simulate") {
            return quill::cli::simulate_command(rest, std::cout);
        }
        if (command == "replay") {
            return quill::cli::replay_command(rest, std::cout);
        }
        if (command == "fix") {
            return quill::cli::fix_command(rest, std::cin, std::cout);
        }
        throw quill::cli::UsageError("unknown command '" + command + "'");
    } catch (const quill::cli::UsageError& e) {
        std::cerr << "quill: " << e.what() << "\n\n" << kUsage;
        return 64;
    } catch (const std::exception& e) {
        std::cerr << "quill: " << e.what() << '\n';
        return 1;
    }
}
