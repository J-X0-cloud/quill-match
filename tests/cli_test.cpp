#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "cli/report.hpp"

namespace quill::cli {
namespace {

TEST(Args, ParsesValuesFlagsAndPositionals) {
    const Args args({"file.itch", "--depth", "7", "--symbol=AAPL", "--verbose", "--", "--raw"},
                    {"depth", "symbol"}, {"verbose"});
    EXPECT_EQ(args.integer("depth", 5, 1, 10), 7);
    EXPECT_EQ(args.value("symbol"), "AAPL");
    EXPECT_TRUE(args.flag("verbose"));
    EXPECT_EQ(args.positionals(), (std::vector<std::string>{"file.itch", "--raw"}));
    EXPECT_EQ(args.value_or("missing", "fallback"), "fallback");
    EXPECT_EQ(args.integer("missing", 42, 0, 100), 42);
}

TEST(Args, ReportsMistakes) {
    EXPECT_THROW(Args({"--nope"}, {}, {}), UsageError);
    EXPECT_THROW(Args({"--depth"}, {"depth"}, {}), UsageError);
    EXPECT_THROW(Args({"--flag=1"}, {}, {"flag"}), UsageError);
    const Args args({"--depth", "abc", "--count", "500"}, {"depth", "count"}, {});
    EXPECT_THROW((void)args.integer("depth", 1, 0, 10), UsageError);
    EXPECT_THROW((void)args.integer("count", 1, 0, 10), UsageError);
}

TEST(Args, SplitList) {
    EXPECT_EQ(split_list("a,b,,c"), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_TRUE(split_list("").empty());
    EXPECT_EQ(split_list("solo"), (std::vector<std::string>{"solo"}));
}

TEST(Report, Formatting) {
    EXPECT_EQ(with_commas(0), "0");
    EXPECT_EQ(with_commas(999), "999");
    EXPECT_EQ(with_commas(1'000), "1,000");
    EXPECT_EQ(with_commas(1'234'567), "1,234,567");
    EXPECT_EQ(rate(4'210'000), "4.21M/s");
    EXPECT_EQ(rate(1'500), "1.5K/s");
    EXPECT_EQ(rate(12), "12/s");
}

class CliRoundTrip : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("quill_cli_test_" +
                 std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".itch");
    }
    void TearDown() override { std::filesystem::remove(path_); }
    std::filesystem::path path_;
};

TEST_F(CliRoundTrip, SimulateWritesFeedThatReplaysCleanly) {
    for (const bool threaded : {false, true}) {
        SimulationOptions options;
        options.orders = 5'000;
        options.warmup = 500;
        options.instruments = 3;
        options.threaded = threaded;
        options.queue_capacity = 64;
        options.itch_out = path_.string();
        std::ostringstream sim_out;
        const SimulationResult result = simulate(options, sim_out);
        EXPECT_EQ(result.latency.count(), 5'000U);
        EXPECT_GT(result.stats.trades, 100U);
        EXPECT_GT(result.itch_messages, 5'000U);
        EXPECT_EQ(std::filesystem::file_size(path_), result.itch_bytes);

        std::ostringstream replay_out;
        const ReplayResult replayed = replay(path_.string(), 3, std::nullopt, replay_out);
        EXPECT_TRUE(replayed.ok) << replayed.error;
        EXPECT_EQ(replayed.messages, result.itch_messages);
        EXPECT_NE(replay_out.str().find("anomalies 0"), std::string::npos) << replay_out.str();
        EXPECT_NE(replay_out.str().find("SYM3"), std::string::npos);
    }
}

TEST_F(CliRoundTrip, ReplayReportsTruncation) {
    SimulationOptions options;
    options.orders = 1'000;
    options.warmup = 0;
    options.instruments = 1;
    options.itch_out = path_.string();
    std::ostringstream sink;
    (void)simulate(options, sink);
    std::filesystem::resize_file(path_, std::filesystem::file_size(path_) - 3);
    const ReplayResult replayed = replay(path_.string(), 1, std::nullopt, sink);
    EXPECT_FALSE(replayed.ok);
    EXPECT_NE(replayed.error.find("truncated"), std::string::npos);
}

TEST(CliCommands, SimulateCommandPrintsSummary) {
    std::ostringstream out;
    EXPECT_EQ(simulate_command({"--orders", "2000", "--warmup", "0", "--instruments", "2"}, out),
              0);
    EXPECT_NE(out.str().find("latency: per command"), std::string::npos);
    EXPECT_NE(out.str().find("99.9"), std::string::npos);
    EXPECT_THROW(simulate_command({"--orders", "0"}, out), UsageError);
    EXPECT_THROW(simulate_command({"extra"}, out), UsageError);
    EXPECT_THROW(simulate_command({"--rate", "1000"}, out), UsageError);
}

TEST(CliCommands, PacedThreadedSimulation) {
    SimulationOptions options;
    options.orders = 2'000;
    options.warmup = 0;
    options.instruments = 1;
    options.threaded = true;
    options.rate = 200'000;
    std::ostringstream out;
    const SimulationResult result = simulate(options, out);
    EXPECT_EQ(result.latency.count(), 2'000U);
    // 2000 commands at 200k/s cannot finish faster than 10 ms.
    EXPECT_GE(result.seconds, 0.009);
}

TEST(CliCommands, ReplayCommandValidatesArguments) {
    std::ostringstream out;
    EXPECT_THROW(replay_command({}, out), UsageError);
    EXPECT_THROW(replay_command({"/definitely/not/here.itch"}, out), UsageError);
}

TEST(CliCommands, FixConsoleRunsAScriptedSession) {
    std::istringstream in(
        "# comment lines and blanks are ignored\n"
        "\n"
        "8=FIX.4.4|35=A|49=ALICE|56=QUILL|34=1|98=0|108=30|\n"
        "8=FIX.4.4|35=D|49=ALICE|56=QUILL|34=2|11=a1|55=MSFT|54=2|38=100|40=2|44=410.25|\n"
        "8=FIX.4.4|35=A|49=BOB|56=QUILL|34=1|98=0|108=30|\n"
        "8=FIX.4.4|35=D|49=BOB|56=QUILL|34=2|11=b1|55=MSFT|54=1|38=100|40=2|44=410.25|\n");
    std::ostringstream out;
    FixConsoleOptions options;
    EXPECT_EQ(run_fix_console(options, in, out), 4U);
    const std::string text = out.str();
    EXPECT_NE(text.find("-> ALICE 8=FIX.4.4|"), std::string::npos);
    EXPECT_NE(text.find("150=F|39=2"), std::string::npos);
    EXPECT_NE(text.find("31=410.25|32=100|"), std::string::npos);
}

TEST(CliCommands, FixCommandRejectsBadOptions) {
    std::istringstream in;
    std::ostringstream out;
    EXPECT_THROW(fix_command({"--delimiter", "comma"}, in, out), UsageError);
    EXPECT_THROW(fix_command({"--symbols", ","}, in, out), UsageError);
    EXPECT_EQ(fix_command({"--symbols", "X,Y", "--strict"}, in, out), 0);
}

}  // namespace
}  // namespace quill::cli
