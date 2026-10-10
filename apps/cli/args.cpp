#include "cli/args.hpp"

#include <charconv>

namespace quill::cli {

Args::Args(const std::vector<std::string>& argv, const std::set<std::string>& value_options,
           const std::set<std::string>& flags) {
    bool options_done = false;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (options_done || arg.size() < 3 || arg.rfind("--", 0) != 0) {
            if (arg == "--") {
                options_done = true;
                continue;
            }
            positionals_.push_back(arg);
            continue;
        }
        std::string name = arg.substr(2);
        std::optional<std::string> inline_value;
        if (const auto eq = name.find('='); eq != std::string::npos) {
            inline_value = name.substr(eq + 1);
            name.erase(eq);
        }
        if (flags.contains(name)) {
            if (inline_value) {
                throw UsageError("option --" + name + " does not take a value");
            }
            flags_.insert(name);
        } else if (value_options.contains(name)) {
            if (inline_value) {
                values_[name] = *inline_value;
            } else if (i + 1 < argv.size()) {
                values_[name] = argv[++i];
            } else {
                throw UsageError("option --" + name + " needs a value");
            }
        } else {
            throw UsageError("unknown option --" + name);
        }
    }
}

std::optional<std::string> Args::value(const std::string& name) const {
    const auto it = values_.find(name);
    return it == values_.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::string Args::value_or(const std::string& name, std::string fallback) const {
    const auto v = value(name);
    return v ? *v : std::move(fallback);
}

std::int64_t Args::integer(const std::string& name, std::int64_t fallback, std::int64_t min,
                           std::int64_t max) const {
    const auto text = value(name);
    if (!text) {
        return fallback;
    }
    std::int64_t parsed = 0;
    const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), parsed);
    if (ec != std::errc{} || end != text->data() + text->size()) {
        throw UsageError("option --" + name + " expects an integer, got '" + *text + "'");
    }
    if (parsed < min || parsed > max) {
        throw UsageError("option --" + name + " must be between " + std::to_string(min) + " and " +
                         std::to_string(max));
    }
    return parsed;
}

std::vector<std::string> split_list(std::string_view text, char separator) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = std::min(text.find(separator, pos), text.size());
        if (end > pos) {
            out.emplace_back(text.substr(pos, end - pos));
        }
        pos = end + 1;
    }
    return out;
}

}  // namespace quill::cli
