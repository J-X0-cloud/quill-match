#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "quill/events.hpp"
#include "quill/order_book.hpp"

namespace quill::cli {

/// Counts what the engine did, for end-of-run summaries.
class StatsListener final : public EventListener {
  public:
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t trades = 0;
    Quantity volume = 0;
    std::uint64_t cancels = 0;
    std::uint64_t replaces = 0;
    std::array<std::uint64_t, 16> rejects_by_reason{};

    void on_accepted(const OrderAccepted&) override { ++accepted; }
    void on_rejected(const OrderRejected& e) override {
        ++rejected;
        ++rejects_by_reason[static_cast<std::size_t>(e.reason)];
    }
    void on_trade(const Trade& e) override {
        ++trades;
        volume += e.quantity;
    }
    void on_cancelled(const OrderCancelled&) override { ++cancels; }
    void on_replaced(const OrderReplaced&) override { ++replaces; }

    void print(std::ostream& out) const;
};

/// 1234567 -> "1,234,567"
[[nodiscard]] std::string with_commas(std::uint64_t value);
/// Human-readable rate such as "4.21M/s".
[[nodiscard]] std::string rate(double per_second);

/// Prints a side-by-side ladder of the top `levels` bids and asks.
void print_ladder(std::ostream& out, const std::vector<LevelSnapshot>& bids,
                  const std::vector<LevelSnapshot>& asks);

}  // namespace quill::cli
