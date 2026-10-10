#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "quill/commands.hpp"
#include "quill/types.hpp"

namespace quill::sim {

/// Knobs for the synthetic order flow. Ratios are probabilities per generated command and
/// are applied in order (cancel, replace, then the new-order mix); whatever is left over
/// becomes plain resting limit orders.
struct FlowConfig {
    std::vector<InstrumentId> instruments;
    std::uint64_t seed = 1;
    Price start_price = 100 * kPriceScale;
    Price tick_size = 100;
    /// Limit prices are drawn within this many ticks either side of the moving mid.
    int price_band_ticks = 10;
    /// Mid moves by one tick (up or down) after this many commands on an instrument.
    int drift_interval = 50;
    Quantity max_quantity = 500;
    std::uint32_t participants = 16;

    double cancel_ratio = 0.30;
    double replace_ratio = 0.10;
    double market_ratio = 0.03;
    double ioc_ratio = 0.05;
    double fok_ratio = 0.02;
    double post_only_ratio = 0.05;
    double iceberg_ratio = 0.03;
    /// Fraction of new orders that request cancel-incoming self-trade prevention.
    double stp_ratio = 0.10;
};

/// Deterministic generator of realistic-looking order flow for benchmarks, simulations
/// and soak tests.
///
/// The generator assigns order ids itself, so it can reference its own earlier orders in
/// cancels and replaces without any feedback from the engine. Some of those requests will
/// target orders that have already traded away; the engine rejects them exactly as it
/// would reject a late cancel from a real client.
class OrderFlowGenerator {
  public:
    explicit OrderFlowGenerator(FlowConfig config);

    [[nodiscard]] Command next();
    /// Generates `count` commands into a vector (useful for pre-building benchmark input).
    [[nodiscard]] std::vector<Command> generate(std::size_t count);

    [[nodiscard]] const FlowConfig& config() const noexcept { return config_; }
    [[nodiscard]] OrderId last_order_id() const noexcept { return next_id_ - 1; }

  private:
    struct Recent {
        OrderId id = kInvalidOrderId;
        Side side = Side::Buy;
        Price price = 0;
        Quantity quantity = 0;
    };

    struct InstrumentState {
        InstrumentId id = 0;
        Price mid = 0;
        int until_drift = 0;
        std::vector<Recent> recent;
    };

    static constexpr std::size_t kRecentWindow = 512;

    [[nodiscard]] double unit() noexcept;
    [[nodiscard]] std::int64_t between(std::int64_t lo, std::int64_t hi) noexcept;
    [[nodiscard]] Price limit_price(const InstrumentState& state, Side side) noexcept;
    void remember(InstrumentState& state, const Recent& order);
    void drift(InstrumentState& state) noexcept;

    [[nodiscard]] Command new_order(InstrumentState& state);
    [[nodiscard]] Command cancel(InstrumentState& state);
    [[nodiscard]] Command replace(InstrumentState& state);

    FlowConfig config_;
    std::mt19937_64 rng_;
    std::vector<InstrumentState> states_;
    OrderId next_id_ = 1;
};

}  // namespace quill::sim
