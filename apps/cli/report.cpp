#include "cli/report.hpp"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <ostream>

#include "quill/fix/message.hpp"

namespace quill::cli {

std::string with_commas(std::uint64_t value) {
    const std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out += ',';
        }
        out += digits[i];
    }
    return out;
}

std::string rate(double per_second) {
    char buffer[32];
    if (per_second >= 1e6) {
        std::snprintf(buffer, sizeof(buffer), "%.2fM/s", per_second / 1e6);
    } else if (per_second >= 1e3) {
        std::snprintf(buffer, sizeof(buffer), "%.1fK/s", per_second / 1e3);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f/s", per_second);
    }
    return buffer;
}

void StatsListener::print(std::ostream& out) const {
    out << "  accepted   " << with_commas(accepted) << '\n'
        << "  rejected   " << with_commas(rejected) << '\n';
    for (std::size_t i = 0; i < rejects_by_reason.size(); ++i) {
        if (rejects_by_reason[i] != 0) {
            out << "    " << std::left << std::setw(48) << to_string(static_cast<RejectReason>(i))
                << std::right << with_commas(rejects_by_reason[i]) << '\n';
        }
    }
    out << "  trades     " << with_commas(trades) << " ("
        << with_commas(static_cast<std::uint64_t>(volume)) << " units)\n"
        << "  cancels    " << with_commas(cancels) << '\n'
        << "  replaces   " << with_commas(replaces) << '\n';
}

void print_ladder(std::ostream& out, const std::vector<LevelSnapshot>& bids,
                  const std::vector<LevelSnapshot>& asks) {
    out << "  " << std::setw(8) << "orders" << std::setw(10) << "bid qty" << std::setw(12) << "bid"
        << " | " << std::left << std::setw(12) << "ask" << std::right << std::setw(10) << "ask qty"
        << std::setw(8) << "orders" << '\n';
    const std::size_t rows = std::max(bids.size(), asks.size());
    for (std::size_t i = 0; i < rows; ++i) {
        out << "  ";
        if (i < bids.size()) {
            out << std::setw(8) << bids[i].orders << std::setw(10) << bids[i].visible
                << std::setw(12) << fix::format_price(bids[i].price);
        } else {
            out << std::setw(30) << "";
        }
        out << " | ";
        if (i < asks.size()) {
            out << std::left << std::setw(12) << fix::format_price(asks[i].price) << std::right
                << std::setw(10) << asks[i].visible << std::setw(8) << asks[i].orders;
        }
        out << '\n';
    }
    if (rows == 0) {
        out << "  (empty book)\n";
    }
}

}  // namespace quill::cli
