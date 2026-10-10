#include "quill/fix/gateway.hpp"

#include <utility>

namespace quill::fix {

namespace {

// SessionRejectReason (373) values.
constexpr int kRequiredTagMissing = 1;
constexpr int kValueIsIncorrect = 5;
constexpr int kIncorrectDataFormat = 6;
constexpr int kInvalidMsgType = 11;

// OrdRejReason (103) values.
constexpr int kOrdRejUnknownSymbol = 1;
constexpr int kOrdRejExceedsLimit = 3;
constexpr int kOrdRejDuplicate = 6;
constexpr int kOrdRejOther = 99;

// CxlRejReason (102) values.
constexpr int kCxlRejUnknownOrder = 1;
constexpr int kCxlRejDuplicateClOrdId = 6;
constexpr int kCxlRejOther = 99;

constexpr char kExecNew = '0';
constexpr char kExecCanceled = '4';
constexpr char kExecReplaced = '5';
constexpr char kExecRejected = '8';
constexpr char kExecTrade = 'F';

constexpr char kStatusNew = '0';
constexpr char kStatusPartial = '1';
constexpr char kStatusFilled = '2';
constexpr char kStatusCanceled = '4';
constexpr char kStatusRejected = '8';

int ord_rej_reason(RejectReason reason) {
    switch (reason) {
        case RejectReason::UnknownInstrument:
            return kOrdRejUnknownSymbol;
        case RejectReason::InvalidQuantity:
        case RejectReason::PriceOutsideCollar:
            return kOrdRejExceedsLimit;
        case RejectReason::DuplicateOrderId:
            return kOrdRejDuplicate;
        default:
            return kOrdRejOther;
    }
}

char status_for(Quantity cum, Quantity leaves) {
    if (leaves == 0) {
        return kStatusFilled;
    }
    return cum > 0 ? kStatusPartial : kStatusNew;
}

}  // namespace

Gateway::Gateway(MatchingEngine& engine, Clock& clock, GatewayConfig config)
    : engine_(&engine), clock_(&clock), config_(std::move(config)) {}

bool Gateway::logged_on(std::string_view session) const {
    const auto it = session_index_.find(std::string(session));
    return it != session_index_.end() && sessions_[it->second].logged_on;
}

// ---------------------------------------------------------------------------------------
// Inbound dispatch and session layer
// ---------------------------------------------------------------------------------------

std::vector<Outbound> Gateway::handle(std::string_view raw) {
    // Anything produced while no message was being handled goes out first.
    std::vector<Outbound> out = std::exchange(backlog_, {});
    out_ = &out;
    struct RestoreSink {
        Gateway* gateway;
        explicit RestoreSink(Gateway* owner) : gateway(owner) {}
        RestoreSink(const RestoreSink&) = delete;
        RestoreSink& operator=(const RestoreSink&) = delete;
        RestoreSink(RestoreSink&&) = delete;
        RestoreSink& operator=(RestoreSink&&) = delete;
        ~RestoreSink() { gateway->out_ = &gateway->backlog_; }
    } restore{this};

    auto parsed = Message::parse(raw, config_.delimiter, config_.strict);
    if (!parsed.message) {
        reject_unaddressable(raw, to_string(*parsed.error));
        return out;
    }
    const Message& m = *parsed.message;
    const auto sender = m.get(tag::kSenderCompId);
    const auto target = m.get(tag::kTargetCompId);
    const auto seq = m.get_int(tag::kMsgSeqNum);
    if (!sender || !target || !seq) {
        reject_unaddressable(raw, "SenderCompID, TargetCompID and MsgSeqNum are required");
        return out;
    }
    if (*target != config_.comp_id) {
        reject_unaddressable(raw, "unknown TargetCompID");
        return out;
    }

    const bool is_logon = m.msg_type() == msg::kLogon;
    auto it = session_index_.find(std::string(*sender));
    if (it == session_index_.end()) {
        if (!is_logon) {
            reject_unaddressable(raw, "not logged on");
            return out;
        }
        Session session;
        session.comp_id = std::string(*sender);
        session.participant = static_cast<ParticipantId>(sessions_.size() + 1);
        sessions_.push_back(std::move(session));
        it = session_index_.emplace(sessions_.back().comp_id, sessions_.size() - 1).first;
    }
    const std::size_t s = it->second;
    Session& session = sessions_[s];
    if (!session.logged_on) {
        if (!is_logon) {
            reject_unaddressable(raw, "not logged on");
            return out;
        }
        // A fresh logon starts a new sequence on both sides.
        session.next_inbound = 1;
        session.next_outbound = 1;
    } else if (is_logon) {
        session_reject(s, m, tag::kMsgType, kValueIsIncorrect, "already logged on");
        ++session.next_inbound;
        return out;
    }

    if (static_cast<std::uint64_t>(*seq) != session.next_inbound) {
        // Resend is not implemented, so any gap or regression ends the session.
        const std::string reason = "MsgSeqNum " + std::to_string(*seq) + " but expected " +
                                   std::to_string(session.next_inbound);
        session.logged_on = true;  // so the logout can be addressed normally
        on_logout(s, reason);
        return out;
    }
    ++session.next_inbound;

    const std::string_view type = m.msg_type();
    if (type == msg::kLogon) {
        on_logon(m, s);
    } else if (type == msg::kHeartbeat) {
        // Nothing to do: receipt alone proves the counterparty is alive.
    } else if (type == msg::kTestRequest) {
        on_test_request(m, s);
    } else if (type == msg::kLogout) {
        on_logout(s, "");
    } else if (type == msg::kNewOrderSingle) {
        on_new_order(m, s);
    } else if (type == msg::kOrderCancelRequest) {
        on_cancel(m, s);
    } else if (type == msg::kOrderCancelReplaceRequest) {
        on_replace(m, s);
    } else {
        session_reject(s, m, tag::kMsgType, kInvalidMsgType, "unsupported MsgType");
    }
    return out;
}

void Gateway::on_logon(const Message& m, std::size_t session) {
    sessions_[session].logged_on = true;
    const std::int64_t heartbeat = m.get_int(tag::kHeartBtInt).value_or(30);
    Builder reply = start(session, msg::kLogon);
    reply.add(98, std::int64_t{0}).add(tag::kHeartBtInt, heartbeat);
    send(session, reply);
}

void Gateway::on_test_request(const Message& m, std::size_t session) {
    Builder reply = start(session, msg::kHeartbeat);
    if (const auto id = m.get(tag::kTestReqId)) {
        reply.add(tag::kTestReqId, *id);
    }
    send(session, reply);
}

void Gateway::on_logout(std::size_t session, std::string_view reason) {
    Builder reply = start(session, msg::kLogout);
    if (!reason.empty()) {
        reply.add(tag::kText, reason);
    }
    send(session, reply);
    sessions_[session].logged_on = false;
}

void Gateway::session_reject(std::size_t session, const Message& m, int ref_tag, int reason,
                             std::string_view text) {
    Builder reply = start(session, msg::kReject);
    if (const auto seq = m.get(tag::kMsgSeqNum)) {
        reply.add(tag::kRefSeqNum, *seq);
    }
    reply.add(tag::kRefTagId, static_cast<std::int64_t>(ref_tag));
    if (const auto type = m.get(tag::kMsgType)) {
        reply.add(tag::kRefMsgType, *type);
    }
    reply.add(tag::kSessionRejectReason, static_cast<std::int64_t>(reason));
    reply.add(tag::kText, text);
    send(session, reply);
}

void Gateway::reject_unaddressable(std::string_view raw, std::string_view text) {
    // Best effort to find out who sent it; the message failed validation, so parse it
    // leniently purely to read the header.
    const auto lenient = Message::parse(raw, config_.delimiter, false);
    std::string sender;
    if (lenient.message) {
        if (const auto comp = lenient.message->get(tag::kSenderCompId)) {
            sender = std::string(*comp);
        }
    }
    const auto it = session_index_.find(sender);
    if (it != session_index_.end() && sessions_[it->second].logged_on && lenient.message) {
        // Garbled message on a live session: reject it without consuming its sequence
        // number, as FIX prescribes for messages that fail validation.
        session_reject(it->second, *lenient.message, 0, kValueIsIncorrect, text);
        return;
    }
    Builder reply(msg::kLogout, config_.delimiter);
    reply.add(tag::kSenderCompId, config_.comp_id);
    if (!sender.empty()) {
        reply.add(tag::kTargetCompId, sender);
    }
    reply.add(tag::kMsgSeqNum, std::int64_t{1})
        .add(tag::kSendingTime, format_utc_timestamp(clock_->now()))
        .add(tag::kText, text);
    out_->push_back(Outbound{sender, reply.finish()});
}

// ---------------------------------------------------------------------------------------
// Application layer
// ---------------------------------------------------------------------------------------

bool Gateway::read_order_fields(const Message& m, std::size_t session, Pending& pending) {
    for (const int required :
         {tag::kClOrdId, tag::kSymbol, tag::kSide, tag::kOrderQty, tag::kOrdType}) {
        if (!m.has(required)) {
            session_reject(session, m, required, kRequiredTagMissing, "required tag missing");
            return false;
        }
    }
    pending.session = session;
    pending.cl_ord_id = std::string(*m.get(tag::kClOrdId));
    pending.symbol = std::string(*m.get(tag::kSymbol));

    const std::string_view side = *m.get(tag::kSide);
    if (side != "1" && side != "2") {
        session_reject(session, m, tag::kSide, kValueIsIncorrect, "Side must be 1 or 2");
        return false;
    }
    pending.side = side == "1" ? Side::Buy : Side::Sell;

    const auto quantity = m.get_int(tag::kOrderQty);
    if (!quantity) {
        session_reject(session, m, tag::kOrderQty, kIncorrectDataFormat,
                       "OrderQty must be an integer");
        return false;
    }
    pending.quantity = *quantity;

    const std::string_view ord_type = *m.get(tag::kOrdType);
    if (ord_type == "1") {
        pending.order_type = OrderType::Market;
    } else if (ord_type == "2") {
        pending.order_type = OrderType::Limit;
        const auto price_text = m.get(tag::kPrice);
        if (!price_text) {
            session_reject(session, m, tag::kPrice, kRequiredTagMissing,
                           "Price is required for limit orders");
            return false;
        }
        const auto price = parse_price(*price_text);
        if (!price) {
            session_reject(session, m, tag::kPrice, kIncorrectDataFormat, "malformed Price");
            return false;
        }
        pending.price = *price;
    } else {
        session_reject(session, m, tag::kOrdType, kValueIsIncorrect,
                       "OrdType must be 1 (market) or 2 (limit)");
        return false;
    }
    return true;
}

std::uint64_t Gateway::track(Pending pending) {
    const std::uint64_t client_tag = next_tag_++;
    pending_.emplace(client_tag, std::move(pending));
    return client_tag;
}

void Gateway::on_new_order(const Message& m, std::size_t session) {
    Pending pending;
    pending.type = RequestType::New;
    if (!read_order_fields(m, session, pending)) {
        return;
    }
    NewOrderRequest request;
    request.side = pending.side;
    request.type = pending.order_type;
    request.price = pending.price;
    request.quantity = pending.quantity;
    request.participant = sessions_[session].participant;
    request.tif = pending.order_type == OrderType::Market ? TimeInForce::ImmediateOrCancel
                                                          : TimeInForce::GoodTillCancel;
    if (const auto tif = m.get(tag::kTimeInForce)) {
        // Day (0) is treated as GTC: the engine has no trading-session calendar.
        if (*tif == "0" || *tif == "1") {
            request.tif = TimeInForce::GoodTillCancel;
        } else if (*tif == "3") {
            request.tif = TimeInForce::ImmediateOrCancel;
        } else if (*tif == "4") {
            request.tif = TimeInForce::FillOrKill;
        } else {
            session_reject(session, m, tag::kTimeInForce, kValueIsIncorrect,
                           "TimeInForce must be 0, 1, 3 or 4");
            return;
        }
    }
    if (const auto inst = m.get(tag::kExecInst)) {
        // ExecInst is a space separated list; '6' is Participate Don't Initiate.
        request.post_only = inst->find('6') != std::string_view::npos;
    }
    if (m.has(tag::kDisplayQty)) {
        const auto display = m.get_int(tag::kDisplayQty);
        if (!display) {
            session_reject(session, m, tag::kDisplayQty, kIncorrectDataFormat,
                           "DisplayQty must be an integer");
            return;
        }
        request.display_quantity = *display;
    }
    if (const auto stp = m.get(tag::kSelfMatchPrevention)) {
        if (*stp == "1") {
            request.stp = SelfTradePrevention::CancelIncoming;
        } else if (*stp == "2") {
            request.stp = SelfTradePrevention::CancelResting;
        } else if (*stp == "3") {
            request.stp = SelfTradePrevention::CancelBoth;
        } else {
            session_reject(session, m, tag::kSelfMatchPrevention, kValueIsIncorrect,
                           "SelfMatchPreventionInstruction must be 1, 2 or 3");
            return;
        }
    }

    Session& owner = sessions_[session];
    if (owner.live.contains(pending.cl_ord_id)) {
        OrderState shadow;
        shadow.session = session;
        shadow.symbol = pending.symbol;
        shadow.side = pending.side;
        shadow.quantity = pending.quantity;
        send_execution(shadow, kInvalidOrderId, kExecRejected, kStatusRejected, pending.cl_ord_id,
                       {}, 0, 0, "duplicate ClOrdID");
        return;
    }
    const auto instrument = engine_->find_instrument(pending.symbol);
    request.instrument = instrument.value_or(0);  // 0 makes the engine reject it
    request.client_tag = track(std::move(pending));
    engine_->submit(request);
    pending_.erase(request.client_tag);
}

void Gateway::on_cancel(const Message& m, std::size_t session) {
    for (const int required : {tag::kClOrdId, tag::kOrigClOrdId, tag::kSymbol, tag::kSide}) {
        if (!m.has(required)) {
            session_reject(session, m, required, kRequiredTagMissing, "required tag missing");
            return;
        }
    }
    Pending pending;
    pending.type = RequestType::Cancel;
    pending.session = session;
    pending.cl_ord_id = std::string(*m.get(tag::kClOrdId));
    pending.orig_cl_ord_id = std::string(*m.get(tag::kOrigClOrdId));
    pending.symbol = std::string(*m.get(tag::kSymbol));

    Session& owner = sessions_[session];
    const auto live = owner.live.find(pending.orig_cl_ord_id);
    if (live == owner.live.end()) {
        send_cancel_reject(pending, kInvalidOrderId, RejectReason::UnknownOrder);
        return;
    }
    const OrderId order_id = live->second;
    const InstrumentId instrument = orders_.at(order_id).instrument;
    const std::uint64_t client_tag = track(std::move(pending));
    engine_->cancel(CancelRequest{instrument, order_id, client_tag});
    pending_.erase(client_tag);
}

void Gateway::on_replace(const Message& m, std::size_t session) {
    if (!m.has(tag::kOrigClOrdId)) {
        session_reject(session, m, tag::kOrigClOrdId, kRequiredTagMissing, "required tag missing");
        return;
    }
    Pending pending;
    pending.type = RequestType::Replace;
    if (!read_order_fields(m, session, pending)) {
        return;
    }
    pending.orig_cl_ord_id = std::string(*m.get(tag::kOrigClOrdId));

    Session& owner = sessions_[session];
    const auto live = owner.live.find(pending.orig_cl_ord_id);
    if (live == owner.live.end()) {
        send_cancel_reject(pending, kInvalidOrderId, RejectReason::UnknownOrder);
        return;
    }
    const OrderId order_id = live->second;
    const OrderState& state = orders_.at(order_id);
    if (pending.cl_ord_id != pending.orig_cl_ord_id && owner.live.contains(pending.cl_ord_id)) {
        send_cancel_reject(pending, order_id, RejectReason::DuplicateOrderId);
        return;
    }
    if (pending.order_type != state.type || pending.side != state.side) {
        session_reject(session, m, tag::kSide, kValueIsIncorrect,
                       "Side and OrdType cannot change on replace");
        return;
    }
    const InstrumentId instrument = state.instrument;
    const Price price = pending.price;
    const Quantity quantity = pending.quantity;
    const std::uint64_t client_tag = track(std::move(pending));
    engine_->replace(ReplaceRequest{instrument, order_id, client_tag, price, quantity});
    pending_.erase(client_tag);
}

// ---------------------------------------------------------------------------------------
// Engine events
// ---------------------------------------------------------------------------------------

void Gateway::on_accepted(const OrderAccepted& event) {
    const auto it = pending_.find(event.client_tag);
    if (it == pending_.end() || it->second.type != RequestType::New) {
        return;  // not one of ours
    }
    const Pending& pending = it->second;
    OrderState state;
    state.session = pending.session;
    state.cl_ord_id = pending.cl_ord_id;
    state.instrument = event.instrument;
    state.symbol = pending.symbol;
    state.side = event.side;
    state.type = event.type;
    state.price = event.price;
    state.quantity = event.quantity;
    state.leaves = event.quantity;
    sessions_[pending.session].live.emplace(pending.cl_ord_id, event.order_id);
    const auto [entry, inserted] = orders_.emplace(event.order_id, std::move(state));
    (void)inserted;
    send_execution(entry->second, event.order_id, kExecNew, kStatusNew, entry->second.cl_ord_id, {},
                   0, 0, {});
}

void Gateway::on_rejected(const OrderRejected& event) {
    const auto it = pending_.find(event.client_tag);
    if (it == pending_.end()) {
        return;
    }
    const Pending& pending = it->second;
    if (pending.type != RequestType::New) {
        send_cancel_reject(pending, event.order_id, event.reason);
        return;
    }
    Builder report = start(pending.session, msg::kExecutionReport);
    report.add(tag::kOrderId, std::string_view("NONE"))
        .add(tag::kClOrdId, pending.cl_ord_id)
        .add(tag::kExecId, "E" + std::to_string(next_exec_id_++))
        .add(tag::kExecType, kExecRejected)
        .add(tag::kOrdStatus, kStatusRejected)
        .add(tag::kOrdRejReason, static_cast<std::int64_t>(ord_rej_reason(event.reason)))
        .add(tag::kSymbol, pending.symbol)
        .add(tag::kSide, pending.side == Side::Buy ? '1' : '2')
        .add(tag::kOrderQty, pending.quantity)
        .add(tag::kLeavesQty, std::int64_t{0})
        .add(tag::kCumQty, std::int64_t{0})
        .add(tag::kAvgPx, std::string_view("0"))
        .add(tag::kText, to_string(event.reason));
    send(pending.session, report);
}

void Gateway::on_trade(const Trade& event) {
    const auto fill = [&](OrderId order_id, Quantity leaves) {
        const auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            return;
        }
        OrderState& state = it->second;
        state.cum_quantity += event.quantity;
        state.leaves = leaves;
        state.notional += static_cast<long double>(event.price) * event.quantity;
        send_execution(state, order_id, kExecTrade, status_for(state.cum_quantity, leaves),
                       state.cl_ord_id, {}, event.price, event.quantity, {});
        if (leaves == 0) {
            forget(order_id);
        }
    };
    fill(event.aggressor_id, event.aggressor_leaves);
    fill(event.resting_id, event.resting_leaves);
}

void Gateway::on_cancelled(const OrderCancelled& event) {
    const auto it = orders_.find(event.order_id);
    if (it == orders_.end()) {
        return;
    }
    OrderState& state = it->second;
    state.leaves = 0;
    const auto request = pending_.find(event.client_tag);
    if (request != pending_.end() && request->second.type == RequestType::Cancel) {
        send_execution(state, event.order_id, kExecCanceled, kStatusCanceled,
                       request->second.cl_ord_id, request->second.orig_cl_ord_id, 0, 0, {});
    } else {
        send_execution(state, event.order_id, kExecCanceled, kStatusCanceled, state.cl_ord_id, {},
                       0, 0, to_string(event.reason));
    }
    forget(event.order_id);
}

void Gateway::on_replaced(const OrderReplaced& event) {
    const auto it = orders_.find(event.order_id);
    const auto request = pending_.find(event.client_tag);
    if (it == orders_.end() || request == pending_.end()) {
        return;
    }
    OrderState& state = it->second;
    const Pending& pending = request->second;
    Session& owner = sessions_[state.session];
    owner.live.erase(state.cl_ord_id);
    owner.live.emplace(pending.cl_ord_id, event.order_id);
    state.cl_ord_id = pending.cl_ord_id;
    state.price = event.price;
    state.quantity = event.quantity;
    state.leaves = event.leaves;
    send_execution(state, event.order_id, kExecReplaced,
                   status_for(state.cum_quantity, state.leaves), state.cl_ord_id,
                   pending.orig_cl_ord_id, 0, 0, {});
}

// ---------------------------------------------------------------------------------------
// Outbound helpers
// ---------------------------------------------------------------------------------------

Builder Gateway::start(std::size_t session, std::string_view msg_type) {
    Session& s = sessions_[session];
    Builder builder(msg_type, config_.delimiter);
    builder.add(tag::kSenderCompId, config_.comp_id)
        .add(tag::kTargetCompId, s.comp_id)
        .add(tag::kMsgSeqNum, static_cast<std::int64_t>(s.next_outbound++))
        .add(tag::kSendingTime, format_utc_timestamp(clock_->now()));
    return builder;
}

void Gateway::send(std::size_t session, const Builder& builder) {
    out_->push_back(Outbound{sessions_[session].comp_id, builder.finish()});
}

void Gateway::send_execution(const OrderState& order, OrderId order_id, char exec_type,
                             char ord_status, std::string_view cl_ord_id,
                             std::string_view orig_cl_ord_id, Price last_px, Quantity last_qty,
                             std::string_view text) {
    Builder report = start(order.session, msg::kExecutionReport);
    report
        .add(tag::kOrderId,
             order_id == kInvalidOrderId ? std::string("NONE") : std::to_string(order_id))
        .add(tag::kClOrdId, cl_ord_id);
    if (!orig_cl_ord_id.empty()) {
        report.add(tag::kOrigClOrdId, orig_cl_ord_id);
    }
    report.add(tag::kExecId, "E" + std::to_string(next_exec_id_++))
        .add(tag::kExecType, exec_type)
        .add(tag::kOrdStatus, ord_status)
        .add(tag::kSymbol, order.symbol)
        .add(tag::kSide, order.side == Side::Buy ? '1' : '2')
        .add(tag::kOrderQty, order.quantity)
        .add(tag::kOrdType, order.type == OrderType::Market ? '1' : '2');
    if (order.type == OrderType::Limit && order.price > 0) {
        report.add(tag::kPrice, format_price(order.price));
    }
    if (exec_type == kExecTrade) {
        report.add(tag::kLastPx, format_price(last_px)).add(tag::kLastQty, last_qty);
    }
    const double avg =
        order.cum_quantity == 0
            ? 0.0
            : static_cast<double>(order.notional / static_cast<long double>(order.cum_quantity) /
                                  static_cast<long double>(kPriceScale));
    report.add(tag::kLeavesQty, order.leaves)
        .add(tag::kCumQty, order.cum_quantity)
        .add(tag::kAvgPx, format_decimal(avg));
    if (!text.empty()) {
        report.add(tag::kText, text);
    }
    send(order.session, report);
}

void Gateway::send_cancel_reject(const Pending& pending, OrderId order_id, RejectReason reason) {
    char status = kStatusRejected;
    if (const auto it = orders_.find(order_id); it != orders_.end()) {
        status = status_for(it->second.cum_quantity, it->second.leaves);
    }
    int cxl_reason = kCxlRejOther;
    if (reason == RejectReason::UnknownOrder) {
        cxl_reason = kCxlRejUnknownOrder;
    } else if (reason == RejectReason::DuplicateOrderId) {
        cxl_reason = kCxlRejDuplicateClOrdId;
    }
    Builder reject = start(pending.session, msg::kOrderCancelReject);
    reject
        .add(tag::kOrderId,
             order_id == kInvalidOrderId ? std::string("NONE") : std::to_string(order_id))
        .add(tag::kClOrdId, pending.cl_ord_id)
        .add(tag::kOrigClOrdId, pending.orig_cl_ord_id)
        .add(tag::kOrdStatus, status)
        .add(tag::kCxlRejResponseTo, pending.type == RequestType::Cancel ? '1' : '2')
        .add(tag::kCxlRejReason, static_cast<std::int64_t>(cxl_reason))
        .add(tag::kText, to_string(reason));
    send(pending.session, reject);
}

void Gateway::forget(OrderId order_id) {
    const auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return;
    }
    sessions_[it->second.session].live.erase(it->second.cl_ord_id);
    orders_.erase(it);
}

}  // namespace quill::fix
