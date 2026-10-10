// Randomized differential test: drive the real engine and the naive reference book with
// the same command stream and require identical fills, outcomes and resting state.

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "reference_book.hpp"
#include "test_support.hpp"

namespace quill::test {
namespace {

class ModelCheck {
  public:
    explicit ModelCheck(std::uint64_t seed) : rng_(seed), engine_(events_, clock_) {
        InstrumentSpec spec;
        spec.symbol = "FUZZ";
        spec.tick_size = 100;
        spec.lot_size = 1;
        instrument_ = engine_.add_instrument(spec);
    }

    void run(int steps) {
        for (int step = 0; step < steps; ++step) {
            const auto roll = uniform(0, 99);
            if (roll < 55) {
                new_order();
            } else if (roll < 75) {
                cancel_order();
            } else {
                replace_order();
            }
            compare(step);
            if (::testing::Test::HasFatalFailure()) {
                return;
            }
        }
    }

    [[nodiscard]] std::size_t fills() const { return model_.fills.size(); }

  private:
    std::int64_t uniform(std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng_);
    }

    Price random_price() { return px(100.00) + uniform(-8, 8) * 100; }

    void new_order() {
        NewOrderRequest request;
        request.instrument = instrument_;
        request.side = uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
        request.quantity = uniform(1, 40);
        request.price = random_price();
        request.participant = static_cast<ParticipantId>(uniform(0, 4));
        if (uniform(0, 9) < 3) {
            request.stp = static_cast<SelfTradePrevention>(uniform(1, 3));
        }
        const auto kind = uniform(0, 9);
        switch (kind) {
            case 0:
                request.tif = TimeInForce::ImmediateOrCancel;
                break;
            case 1:
                request.tif = TimeInForce::FillOrKill;
                break;
            case 2:
                request.type = OrderType::Market;
                request.tif =
                    uniform(0, 1) == 0 ? TimeInForce::ImmediateOrCancel : TimeInForce::FillOrKill;
                break;
            case 3:
                request.post_only = true;
                break;
            case 4:
                request.quantity = uniform(20, 120);
                request.display_quantity = uniform(1, 15);
                break;
            default:
                break;
        }
        const OrderId id = engine_.submit(request);
        const bool model_accepted = model_.submit(request, next_id_);
        ASSERT_EQ(id != kInvalidOrderId, model_accepted)
            << "acceptance mismatch for order kind " << kind;
        if (id != kInvalidOrderId) {
            ASSERT_EQ(id, next_id_);
            ++next_id_;
        }
    }

    OrderId pick_live() {
        const auto ids = model_.live_ids();
        if (ids.empty() || uniform(0, 19) == 0) {
            return next_id_ + 1000;  // occasionally target an order that does not exist
        }
        return ids[static_cast<std::size_t>(uniform(0, static_cast<std::int64_t>(ids.size()) - 1))];
    }

    void cancel_order() {
        const OrderId id = pick_live();
        const auto rejects_before = events_.rejected.size();
        engine_.cancel(CancelRequest{instrument_, id, 0});
        const bool model_ok = model_.cancel(id);
        ASSERT_EQ(events_.rejected.size() == rejects_before, model_ok);
    }

    void replace_order() {
        const OrderId id = pick_live();
        const Price price = uniform(0, 2) == 0 ? random_price() : current_price(id);
        const Quantity quantity = uniform(1, 130);
        const auto rejects_before = events_.rejected.size();
        engine_.replace(ReplaceRequest{instrument_, id, 0, price, quantity});
        const bool model_ok = model_.replace(id, price, quantity);
        ASSERT_EQ(events_.rejected.size() == rejects_before, model_ok)
            << "replace mismatch on order " << id;
    }

    Price current_price(OrderId id) const {
        const Order* order = engine_.book(instrument_)->find(id);
        return order != nullptr ? order->price : px(100.00);
    }

    void compare(int step) {
        ASSERT_EQ(events_.trades.size(), model_.fills.size()) << "step " << step;
        for (std::size_t i = checked_fills_; i < model_.fills.size(); ++i) {
            const Trade& t = events_.trades[i];
            const auto& f = model_.fills[i];
            ASSERT_EQ(t.aggressor_id, f.aggressor) << "fill " << i << " step " << step;
            ASSERT_EQ(t.resting_id, f.resting) << "fill " << i << " step " << step;
            ASSERT_EQ(t.price, f.price) << "fill " << i << " step " << step;
            ASSERT_EQ(t.quantity, f.quantity) << "fill " << i << " step " << step;
        }
        checked_fills_ = model_.fills.size();

        const OrderBook& book = *engine_.book(instrument_);
        for (Side side : {Side::Buy, Side::Sell}) {
            const auto expected = model_.orders(side);
            std::vector<const Order*> actual;
            book.for_each_order(side, [&](const Order& o) { actual.push_back(&o); });
            ASSERT_EQ(actual.size(), expected.size()) << "step " << step;
            for (std::size_t i = 0; i < expected.size(); ++i) {
                ASSERT_EQ(actual[i]->id, expected[i].id) << "queue position " << i;
                ASSERT_EQ(actual[i]->price, expected[i].price);
                ASSERT_EQ(actual[i]->visible, expected[i].visible);
                ASSERT_EQ(actual[i]->hidden, expected[i].hidden);
                ASSERT_EQ(actual[i]->executed, expected[i].executed);
            }
        }
        const auto problem = book.check_invariants();
        ASSERT_FALSE(problem.has_value()) << *problem << " at step " << step;
    }

    std::mt19937_64 rng_;
    RecordingListener events_;
    ManualClock clock_;
    MatchingEngine engine_;
    InstrumentId instrument_ = 0;
    ReferenceBook model_;
    OrderId next_id_ = 1;
    std::size_t checked_fills_ = 0;
};

class BookModelTest : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(BookModelTest, EngineMatchesReferenceModel) {
    ModelCheck check(GetParam());
    check.run(3'000);
    EXPECT_GT(check.fills(), 100U) << "generator should produce plenty of trades";
}

INSTANTIATE_TEST_SUITE_P(Seeds, BookModelTest,
                         ::testing::Values(1ULL, 2ULL, 3ULL, 42ULL, 1337ULL, 2026ULL, 90210ULL,
                                           0xC0FFEEULL));

}  // namespace
}  // namespace quill::test
