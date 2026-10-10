#include "quill/sim/order_flow.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace quill::sim {

OrderFlowGenerator::OrderFlowGenerator(FlowConfig config)
    : config_(std::move(config)), rng_(config_.seed) {
    if (config_.instruments.empty()) {
        throw std::invalid_argument("order flow needs at least one instrument");
    }
    if (config_.tick_size <= 0 || config_.max_quantity <= 0 || config_.price_band_ticks <= 0) {
        throw std::invalid_argument("order flow tick, quantity and band must be positive");
    }
    for (const InstrumentId id : config_.instruments) {
        InstrumentState state;
        state.id = id;
        state.mid = config_.start_price - config_.start_price % config_.tick_size;
        state.until_drift = config_.drift_interval;
        state.recent.reserve(kRecentWindow);
        states_.push_back(std::move(state));
    }
}

double OrderFlowGenerator::unit() noexcept {
    // 53 random mantissa bits: uniform in [0, 1).
    return static_cast<double>(rng_() >> 11U) * 0x1.0p-53;
}

std::int64_t OrderFlowGenerator::between(std::int64_t lo, std::int64_t hi) noexcept {
    const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
    return lo + static_cast<std::int64_t>(rng_() % span);
}

Price OrderFlowGenerator::limit_price(const InstrumentState& state, Side side) noexcept {
    // Skew prices towards the passive side so most orders rest and a minority cross,
    // which gives a book with depth plus a steady trickle of trades.
    const std::int64_t offset = between(-2, config_.price_band_ticks);
    const Price delta = offset * config_.tick_size;
    const Price price = side == Side::Buy ? state.mid - delta : state.mid + delta;
    return std::max(price, config_.tick_size);
}

void OrderFlowGenerator::remember(InstrumentState& state, const Recent& order) {
    if (state.recent.size() < kRecentWindow) {
        state.recent.push_back(order);
    } else {
        state.recent[static_cast<std::size_t>(rng_() % kRecentWindow)] = order;
    }
}

void OrderFlowGenerator::drift(InstrumentState& state) noexcept {
    if (config_.drift_interval <= 0 || --state.until_drift > 0) {
        return;
    }
    state.until_drift = config_.drift_interval;
    const Price step = (rng_() & 1U) != 0 ? config_.tick_size : -config_.tick_size;
    state.mid = std::max(state.mid + step, config_.tick_size * (config_.price_band_ticks + 4));
}

Command OrderFlowGenerator::next() {
    InstrumentState& state = states_[static_cast<std::size_t>(rng_() % states_.size())];
    drift(state);
    double roll = unit();
    if (!state.recent.empty()) {
        if (roll < config_.cancel_ratio) {
            return cancel(state);
        }
        roll -= config_.cancel_ratio;
        if (roll < config_.replace_ratio) {
            return replace(state);
        }
    }
    return new_order(state);
}

std::vector<Command> OrderFlowGenerator::generate(std::size_t count) {
    std::vector<Command> commands;
    commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        commands.push_back(next());
    }
    return commands;
}

Command OrderFlowGenerator::new_order(InstrumentState& state) {
    NewOrderRequest request;
    request.instrument = state.id;
    request.order_id = next_id_++;
    request.client_tag = request.order_id;
    request.side = (rng_() & 1U) != 0 ? Side::Buy : Side::Sell;
    request.participant = static_cast<ParticipantId>(1 + rng_() % config_.participants);
    request.quantity = between(1, config_.max_quantity);
    request.price = limit_price(state, request.side);
    if (unit() < config_.stp_ratio) {
        request.stp = SelfTradePrevention::CancelIncoming;
    }

    double roll = unit();
    const auto pick = [&roll](double ratio) {
        if (roll < ratio) {
            return true;
        }
        roll -= ratio;
        return false;
    };
    if (pick(config_.market_ratio)) {
        request.type = OrderType::Market;
        request.tif = TimeInForce::ImmediateOrCancel;
        request.price = 0;
    } else if (pick(config_.ioc_ratio)) {
        request.tif = TimeInForce::ImmediateOrCancel;
    } else if (pick(config_.fok_ratio)) {
        request.tif = TimeInForce::FillOrKill;
    } else if (pick(config_.post_only_ratio)) {
        request.post_only = true;
    } else if (pick(config_.iceberg_ratio) && request.quantity >= 20) {
        request.display_quantity = std::max<Quantity>(1, request.quantity / 10);
    }

    if (request.type == OrderType::Limit && request.tif == TimeInForce::GoodTillCancel) {
        remember(state, Recent{request.order_id, request.side, request.price, request.quantity});
    }
    return Command{request, 0};
}

Command OrderFlowGenerator::cancel(InstrumentState& state) {
    const std::size_t index = static_cast<std::size_t>(rng_() % state.recent.size());
    const Recent target = state.recent[index];
    // Forget it so we do not keep cancelling the same order.
    state.recent[index] = state.recent.back();
    state.recent.pop_back();
    return Command{CancelRequest{state.id, target.id, target.id}, 0};
}

Command OrderFlowGenerator::replace(InstrumentState& state) {
    Recent& target = state.recent[static_cast<std::size_t>(rng_() % state.recent.size())];
    ReplaceRequest request;
    request.instrument = state.id;
    request.order_id = target.id;
    request.client_tag = target.id;
    if ((rng_() & 1U) != 0) {
        // Size down in place: keeps queue priority.
        request.price = target.price;
        request.quantity = std::max<Quantity>(1, target.quantity / 2);
    } else {
        request.price = limit_price(state, target.side);
        request.quantity = between(1, config_.max_quantity);
    }
    target.price = request.price;
    target.quantity = request.quantity;
    return Command{request, 0};
}

}  // namespace quill::sim
