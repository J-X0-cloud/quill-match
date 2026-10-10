#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quill/clock.hpp"
#include "quill/events.hpp"
#include "quill/fix/message.hpp"
#include "quill/matching_engine.hpp"

namespace quill::fix {

struct GatewayConfig {
    /// Our CompID: inbound TargetCompID must match it, outbound SenderCompID uses it.
    std::string comp_id = "QUILL";
    char delimiter = kSoh;
    /// Enforce BodyLength and CheckSum on inbound messages.
    bool strict = true;
};

/// One outbound message and the session (counterparty CompID) it is addressed to.
struct Outbound {
    std::string session;
    std::string text;
};

/// Order-entry gateway translating FIX-style messages into engine commands and engine
/// events into execution reports.
///
/// Supported inbound messages: Logon (A), Heartbeat (0), TestRequest (1), Logout (5),
/// NewOrderSingle (D), OrderCancelRequest (F) and OrderCancelReplaceRequest (G). Outbound
/// it produces ExecutionReport (8), OrderCancelReject (9), session-level Reject (3) and
/// the session administrative messages.
///
/// The gateway must be the engine's listener (directly or via an EventFanout) because
/// execution reports are generated from engine events. Matching is synchronous, so every
/// report caused by an inbound message, including fills for passive orders owned by other
/// sessions, is returned from the handle() call that caused it.
class Gateway final : public EventListener {
  public:
    Gateway(MatchingEngine& engine, Clock& clock, GatewayConfig config = {});

    // The output sink points into the object itself, so it must stay put.
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;
    Gateway(Gateway&&) = delete;
    Gateway& operator=(Gateway&&) = delete;
    ~Gateway() override = default;

    /// Processes one inbound message and returns everything that must be sent.
    [[nodiscard]] std::vector<Outbound> handle(std::string_view raw);

    /// Returns messages generated outside handle(), e.g. fills caused by commands sent to
    /// the engine directly. handle() also returns them ahead of its own output.
    [[nodiscard]] std::vector<Outbound> drain() { return std::exchange(backlog_, {}); }

    [[nodiscard]] bool logged_on(std::string_view session) const;
    [[nodiscard]] std::size_t open_orders() const noexcept { return orders_.size(); }
    [[nodiscard]] const GatewayConfig& config() const noexcept { return config_; }

    void on_accepted(const OrderAccepted& event) override;
    void on_rejected(const OrderRejected& event) override;
    void on_trade(const Trade& event) override;
    void on_cancelled(const OrderCancelled& event) override;
    void on_replaced(const OrderReplaced& event) override;

  private:
    struct Session {
        std::string comp_id;
        ParticipantId participant = 0;
        std::uint64_t next_inbound = 1;
        std::uint64_t next_outbound = 1;
        bool logged_on = false;
        /// Live orders by their current ClOrdID.
        std::unordered_map<std::string, OrderId> live;
    };

    struct OrderState {
        std::size_t session = 0;
        std::string cl_ord_id;
        InstrumentId instrument = 0;
        std::string symbol;
        Side side = Side::Buy;
        OrderType type = OrderType::Limit;
        Price price = 0;
        Quantity quantity = 0;
        Quantity cum_quantity = 0;
        Quantity leaves = 0;
        long double notional = 0;  // sum of price * quantity over fills, for AvgPx
    };

    enum class RequestType : std::uint8_t { New, Cancel, Replace };

    /// Context for a request in flight inside the engine, keyed by the client tag the
    /// gateway put on the engine command.
    struct Pending {
        RequestType type = RequestType::New;
        std::size_t session = 0;
        std::string cl_ord_id;
        std::string orig_cl_ord_id;
        std::string symbol;
        Side side = Side::Buy;
        OrderType order_type = OrderType::Limit;
        Price price = 0;
        Quantity quantity = 0;
    };

    // Session layer.
    void on_logon(const Message& m, std::size_t session);
    void on_test_request(const Message& m, std::size_t session);
    void on_logout(std::size_t session, std::string_view reason);
    void session_reject(std::size_t session, const Message& m, int ref_tag, int reason,
                        std::string_view text);
    void reject_unaddressable(std::string_view raw, std::string_view text);

    // Application layer.
    void on_new_order(const Message& m, std::size_t session);
    void on_cancel(const Message& m, std::size_t session);
    void on_replace(const Message& m, std::size_t session);
    /// Validates the fields shared by D and G. Returns false after sending a reject.
    bool read_order_fields(const Message& m, std::size_t session, Pending& pending);
    std::uint64_t track(Pending pending);

    // Outbound helpers.
    Builder start(std::size_t session, std::string_view msg_type);
    void send(std::size_t session, const Builder& builder);
    void send_execution(const OrderState& order, OrderId order_id, char exec_type, char ord_status,
                        std::string_view cl_ord_id, std::string_view orig_cl_ord_id, Price last_px,
                        Quantity last_qty, std::string_view text);
    void send_cancel_reject(const Pending& pending, OrderId order_id, RejectReason reason);
    void forget(OrderId order_id);

    MatchingEngine* engine_;
    Clock* clock_;
    GatewayConfig config_;
    std::vector<Session> sessions_;
    std::unordered_map<std::string, std::size_t> session_index_;
    std::unordered_map<OrderId, OrderState> orders_;
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::uint64_t next_tag_ = 1;
    std::uint64_t next_exec_id_ = 1;
    std::vector<Outbound> backlog_;
    std::vector<Outbound>* out_ = &backlog_;
};

}  // namespace quill::fix
