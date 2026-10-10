#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "quill/clock.hpp"
#include "quill/events.hpp"
#include "quill/matching_engine.hpp"

namespace quill::test {

/// Captures every event so tests can assert on exact sequences.
class RecordingListener final : public EventListener {
  public:
    std::vector<OrderAccepted> accepted;
    std::vector<OrderRejected> rejected;
    std::vector<Trade> trades;
    std::vector<OrderCancelled> cancelled;
    std::vector<OrderReplaced> replaced;
    std::vector<BookOrderAdded> book_adds;
    std::vector<BookOrderExecuted> book_executes;
    std::vector<BookOrderReduced> book_reduces;
    std::vector<BookOrderDeleted> book_deletes;
    /// Compact one-letter-per-event log, useful for asserting event ordering.
    std::string sequence;

    void on_accepted(const OrderAccepted& e) override {
        accepted.push_back(e);
        sequence += 'A';
    }
    void on_rejected(const OrderRejected& e) override {
        rejected.push_back(e);
        sequence += 'R';
    }
    void on_trade(const Trade& e) override {
        trades.push_back(e);
        sequence += 'T';
    }
    void on_cancelled(const OrderCancelled& e) override {
        cancelled.push_back(e);
        sequence += 'C';
    }
    void on_replaced(const OrderReplaced& e) override {
        replaced.push_back(e);
        sequence += 'U';
    }
    void on_book_add(const BookOrderAdded& e) override {
        book_adds.push_back(e);
        sequence += 'a';
    }
    void on_book_execute(const BookOrderExecuted& e) override {
        book_executes.push_back(e);
        sequence += 'e';
    }
    void on_book_reduce(const BookOrderReduced& e) override {
        book_reduces.push_back(e);
        sequence += 'x';
    }
    void on_book_delete(const BookOrderDeleted& e) override {
        book_deletes.push_back(e);
        sequence += 'd';
    }

    void clear() { *this = RecordingListener{}; }

    [[nodiscard]] Quantity traded_volume() const {
        Quantity total = 0;
        for (const Trade& t : trades) {
            total += t.quantity;
        }
        return total;
    }
};

/// Price helper: whole currency units to fixed point, e.g. px(100.25).
constexpr Price px(double value) {
    return static_cast<Price>(value * static_cast<double>(kPriceScale) + (value >= 0 ? 0.5 : -0.5));
}

/// Engine with a single instrument and a deterministic clock.
class EngineFixture : public ::testing::Test {
  protected:
    EngineFixture() : engine(events, clock) {
        InstrumentSpec spec;
        spec.symbol = "TEST";
        spec.tick_size = 100;
        spec.lot_size = 1;
        spec.max_order_quantity = 1'000'000;
        instrument = engine.add_instrument(spec);
    }

    OrderId limit(Side side, Price price, Quantity quantity, ParticipantId participant = 0,
                  TimeInForce tif = TimeInForce::GoodTillCancel) {
        NewOrderRequest request;
        request.instrument = instrument;
        request.side = side;
        request.type = OrderType::Limit;
        request.tif = tif;
        request.price = price;
        request.quantity = quantity;
        request.participant = participant;
        return engine.submit(request);
    }

    OrderId buy(Price price, Quantity quantity) { return limit(Side::Buy, price, quantity); }
    OrderId sell(Price price, Quantity quantity) { return limit(Side::Sell, price, quantity); }

    OrderId market(Side side, Quantity quantity, TimeInForce tif = TimeInForce::ImmediateOrCancel) {
        NewOrderRequest request;
        request.instrument = instrument;
        request.side = side;
        request.type = OrderType::Market;
        request.tif = tif;
        request.quantity = quantity;
        return engine.submit(request);
    }

    OrderId iceberg(Side side, Price price, Quantity quantity, Quantity display) {
        NewOrderRequest request;
        request.instrument = instrument;
        request.side = side;
        request.price = price;
        request.quantity = quantity;
        request.display_quantity = display;
        return engine.submit(request);
    }

    void cancel(OrderId id) { engine.cancel(CancelRequest{instrument, id, 0}); }

    void replace(OrderId id, Price price, Quantity quantity) {
        engine.replace(ReplaceRequest{instrument, id, 0, price, quantity});
    }

    [[nodiscard]] const OrderBook& book() const { return *engine.book(instrument); }

    void expect_consistent() const {
        const auto problem = book().check_invariants();
        EXPECT_FALSE(problem.has_value()) << *problem;
    }

    RecordingListener events;
    ManualClock clock{1'000, 1};
    MatchingEngine engine;
    InstrumentId instrument = 0;
};

}  // namespace quill::test
