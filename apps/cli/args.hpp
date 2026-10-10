#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace quill::cli {

/// Thrown for anything the user typed wrong; main() prints it with the usage text.
class UsageError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Minimal GNU-style option parser: `--name value`, `--name=value`, boolean `--flag`, and
/// positional arguments. Each subcommand declares its options up front so typos are
/// reported instead of silently ignored.
class Args {
  public:
    /// `value_options` take a value, `flags` do not.
    Args(const std::vector<std::string>& argv, const std::set<std::string>& value_options,
         const std::set<std::string>& flags);

    [[nodiscard]] bool flag(const std::string& name) const { return flags_.contains(name); }
    [[nodiscard]] std::optional<std::string> value(const std::string& name) const;
    [[nodiscard]] std::string value_or(const std::string& name, std::string fallback) const;

    /// Integer option with inclusive bounds.
    [[nodiscard]] std::int64_t integer(const std::string& name, std::int64_t fallback,
                                       std::int64_t min, std::int64_t max) const;

    [[nodiscard]] const std::vector<std::string>& positionals() const noexcept {
        return positionals_;
    }

  private:
    std::map<std::string, std::string> values_;
    std::set<std::string> flags_;
    std::vector<std::string> positionals_;
};

/// Splits "a,b,,c" into {"a", "b", "c"}.
[[nodiscard]] std::vector<std::string> split_list(std::string_view text, char separator = ',');

}  // namespace quill::cli
