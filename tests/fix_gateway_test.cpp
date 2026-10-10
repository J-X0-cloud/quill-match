#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "quill/fix/gateway.hpp"
#include "test_support.hpp"

namespace quill::test {
namespace {

using fix::Builder;
using fix::Message;
using fix::Outbound;
namespace tag = fix::tag;

/// A scripted counterparty that keeps its own sequence numbers.
class Client {
  public:
    explicit Client(std::string comp_id) : comp_id_(std::move(comp_id)) {}

    std::string build(std::string_view type,
                      const std::vector<std::pair<int, std::string>>& fields) {
        return build_with_seq(type, fields, seq_++);
    }

    std::string build_with_seq(std::string_view type,
                               const std::vector<std::pair<int, std::string>>& fields,
                               std::uint64_t seq) const {
        Builder builder(type);
        builder.add(tag::kSenderCompId, comp_id_)
            .add(tag::kTargetCompId, std::string_view("QUILL"))
            .add(tag::kMsgSeqNum, static_cast<std::int64_t>(seq))
            .add(tag::kSendingTime, std::string_view("20260101-00:00:00.000"));
        for (const auto& [t, v] : fields) {
            builder.add(t, v);
        }
        return builder.finish();
    }

    [[nodiscard]] const std::string& comp_id() const { return comp_id_; }
    void skip_sequence() { ++seq_; }

  private:
    std::string comp_id_;
    std::uint64_t seq_ = 1;
};

class GatewayTest : public ::testing::Test {
  protected:
    GatewayTest() : engine(fanout, clock), gateway(engine, clock) {
        fanout.add(gateway);
        fanout.add(events);
        InstrumentSpec spec;
        spec.symbol = "AAPL";
        engine.add_instrument(spec);
        spec.symbol = "MSFT";
        engine.add_instrument(spec);
    }

    /// Sends a message and parses every reply strictly, which also proves our outbound
    /// envelope (BodyLength, CheckSum) is correct.
    std::vector<std::pair<std::string, Message>> send(const std::string& raw) {
        std::vector<std::pair<std::string, Message>> replies;
        for (const Outbound& o : gateway.handle(raw)) {
            auto parsed = Message::parse(o.text);
            EXPECT_TRUE(parsed.message.has_value())
                << "gateway produced an invalid message: " << o.text;
            if (parsed.message) {
                EXPECT_EQ(parsed.message->get(tag::kTargetCompId).value_or(""), o.session);
                replies.emplace_back(o.session, std::move(*parsed.message));
            }
        }
        return replies;
    }

    void logon(Client& client) {
        const auto replies = send(client.build(fix::msg::kLogon, {{98, "0"}, {108, "30"}}));
        ASSERT_EQ(replies.size(), 1U);
        ASSERT_EQ(replies[0].second.msg_type(), fix::msg::kLogon);
        ASSERT_TRUE(gateway.logged_on(client.comp_id()));
    }

    std::vector<std::pair<std::string, Message>> new_order(
        Client& client, const std::string& cl_ord_id, const std::string& side,
        const std::string& qty, const std::string& price,
        std::vector<std::pair<int, std::string>> extra = {}) {
        std::vector<std::pair<int, std::string>> fields = {
            {tag::kClOrdId, cl_ord_id},
            {tag::kSymbol, "AAPL"},
            {tag::kSide, side},
            {tag::kOrderQty, qty},
            {tag::kOrdType, price.empty() ? "1" : "2"}};
        if (!price.empty()) {
            fields.emplace_back(tag::kPrice, price);
        }
        fields.insert(fields.end(), extra.begin(), extra.end());
        return send(client.build(fix::msg::kNewOrderSingle, fields));
    }

    static std::string field(const Message& m, int t) {
        return std::string(m.get(t).value_or("<missing>"));
    }

    test::RecordingListener events;
    EventFanout fanout;
    ManualClock clock{1'772'600'767'089'000'000ULL, 1'000'000};
    MatchingEngine engine;
    fix::Gateway gateway;
    Client alice{"ALICE"};
    Client bob{"BOB"};
};

TEST_F(GatewayTest, LogonIsAcknowledgedWithSequenceOne) {
    const auto replies = send(alice.build(fix::msg::kLogon, {{98, "0"}, {108, "15"}}));
    ASSERT_EQ(replies.size(), 1U);
    const Message& m = replies[0].second;
    EXPECT_EQ(replies[0].first, "ALICE");
    EXPECT_EQ(field(m, tag::kSenderCompId), "QUILL");
    EXPECT_EQ(field(m, tag::kMsgSeqNum), "1");
    EXPECT_EQ(field(m, tag::kHeartBtInt), "15");
    EXPECT_EQ(field(m, tag::kSendingTime), "20260304-05:06:07.089");
}

TEST_F(GatewayTest, MessagesBeforeLogonAreRefused) {
    const auto replies = new_order(alice, "1", "1", "100", "10.00");
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kLogout);
    EXPECT_EQ(field(replies[0].second, tag::kText), "not logged on");
    EXPECT_TRUE(events.accepted.empty());
}

TEST_F(GatewayTest, NewOrderIsAcknowledged) {
    logon(alice);
    const auto replies = new_order(alice, "ord-1", "1", "100", "189.50");
    ASSERT_EQ(replies.size(), 1U);
    const Message& ack = replies[0].second;
    EXPECT_EQ(ack.msg_type(), fix::msg::kExecutionReport);
    EXPECT_EQ(field(ack, tag::kExecType), "0");
    EXPECT_EQ(field(ack, tag::kOrdStatus), "0");
    EXPECT_EQ(field(ack, tag::kClOrdId), "ord-1");
    EXPECT_EQ(field(ack, tag::kOrderId), "1");
    EXPECT_EQ(field(ack, tag::kPrice), "189.50");
    EXPECT_EQ(field(ack, tag::kLeavesQty), "100");
    EXPECT_EQ(field(ack, tag::kCumQty), "0");
    EXPECT_EQ(field(ack, tag::kMsgSeqNum), "2");
    EXPECT_EQ(gateway.open_orders(), 1U);
}

TEST_F(GatewayTest, CrossingOrdersReportFillsToBothSessions) {
    logon(alice);
    logon(bob);
    new_order(alice, "a1", "2", "100", "50.00");
    const auto replies = new_order(bob, "b1", "1", "60", "50.10");
    // Bob: ack + fill. Alice: fill on her resting order.
    ASSERT_EQ(replies.size(), 3U);
    EXPECT_EQ(replies[0].first, "BOB");
    EXPECT_EQ(field(replies[0].second, tag::kExecType), "0");

    const Message& bob_fill = replies[1].second;
    EXPECT_EQ(replies[1].first, "BOB");
    EXPECT_EQ(field(bob_fill, tag::kExecType), "F");
    EXPECT_EQ(field(bob_fill, tag::kOrdStatus), "2");
    EXPECT_EQ(field(bob_fill, tag::kLastPx), "50.00");
    EXPECT_EQ(field(bob_fill, tag::kLastQty), "60");
    EXPECT_EQ(field(bob_fill, tag::kAvgPx), "50");

    const Message& alice_fill = replies[2].second;
    EXPECT_EQ(replies[2].first, "ALICE");
    EXPECT_EQ(field(alice_fill, tag::kClOrdId), "a1");
    EXPECT_EQ(field(alice_fill, tag::kOrdStatus), "1");
    EXPECT_EQ(field(alice_fill, tag::kCumQty), "60");
    EXPECT_EQ(field(alice_fill, tag::kLeavesQty), "40");
    EXPECT_EQ(gateway.open_orders(), 1U);
}

TEST_F(GatewayTest, AveragePriceAcrossFills) {
    logon(alice);
    logon(bob);
    new_order(alice, "a1", "2", "10", "10.00");
    new_order(alice, "a2", "2", "30", "10.20");
    const auto replies = new_order(bob, "b1", "1", "40", "10.20");
    const Message& last_bob = replies[3].second;  // ack, fill, alice fill, fill, alice fill
    ASSERT_EQ(replies.size(), 5U);
    EXPECT_EQ(replies[3].first, "BOB");
    EXPECT_EQ(field(last_bob, tag::kCumQty), "40");
    EXPECT_EQ(field(last_bob, tag::kAvgPx), "10.15");  // (10*10.00 + 30*10.20) / 40
}

TEST_F(GatewayTest, CancelRequestCancelsOrder) {
    logon(alice);
    new_order(alice, "a1", "1", "100", "10.00");
    const auto replies = send(alice.build(fix::msg::kOrderCancelRequest, {{tag::kClOrdId, "c1"},
                                                                          {tag::kOrigClOrdId, "a1"},
                                                                          {tag::kSymbol, "AAPL"},
                                                                          {tag::kSide, "1"}}));
    ASSERT_EQ(replies.size(), 1U);
    const Message& m = replies[0].second;
    EXPECT_EQ(field(m, tag::kExecType), "4");
    EXPECT_EQ(field(m, tag::kOrdStatus), "4");
    EXPECT_EQ(field(m, tag::kClOrdId), "c1");
    EXPECT_EQ(field(m, tag::kOrigClOrdId), "a1");
    EXPECT_EQ(field(m, tag::kLeavesQty), "0");
    EXPECT_EQ(gateway.open_orders(), 0U);
    EXPECT_FALSE(engine.book(1)->best_bid().has_value());
}

TEST_F(GatewayTest, CancelUnknownOrderIsRejected) {
    logon(alice);
    const auto replies =
        send(alice.build(fix::msg::kOrderCancelRequest, {{tag::kClOrdId, "c1"},
                                                         {tag::kOrigClOrdId, "nope"},
                                                         {tag::kSymbol, "AAPL"},
                                                         {tag::kSide, "1"}}));
    ASSERT_EQ(replies.size(), 1U);
    const Message& m = replies[0].second;
    EXPECT_EQ(m.msg_type(), fix::msg::kOrderCancelReject);
    EXPECT_EQ(field(m, tag::kCxlRejReason), "1");
    EXPECT_EQ(field(m, tag::kCxlRejResponseTo), "1");
    EXPECT_EQ(field(m, tag::kOrdStatus), "8");
}

TEST_F(GatewayTest, SessionsCannotCancelEachOthersOrders) {
    logon(alice);
    logon(bob);
    new_order(alice, "shared-id", "1", "100", "10.00");
    const auto replies =
        send(bob.build(fix::msg::kOrderCancelRequest, {{tag::kClOrdId, "c1"},
                                                       {tag::kOrigClOrdId, "shared-id"},
                                                       {tag::kSymbol, "AAPL"},
                                                       {tag::kSide, "1"}}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kOrderCancelReject);
    EXPECT_EQ(gateway.open_orders(), 1U);
}

TEST_F(GatewayTest, ReplaceUpdatesClOrdIdChain) {
    logon(alice);
    new_order(alice, "a1", "1", "100", "10.00");
    auto replies =
        send(alice.build(fix::msg::kOrderCancelReplaceRequest, {{tag::kClOrdId, "a2"},
                                                                {tag::kOrigClOrdId, "a1"},
                                                                {tag::kSymbol, "AAPL"},
                                                                {tag::kSide, "1"},
                                                                {tag::kOrderQty, "50"},
                                                                {tag::kOrdType, "2"},
                                                                {tag::kPrice, "10.00"}}));
    ASSERT_EQ(replies.size(), 1U);
    const Message& m = replies[0].second;
    EXPECT_EQ(field(m, tag::kExecType), "5");
    EXPECT_EQ(field(m, tag::kClOrdId), "a2");
    EXPECT_EQ(field(m, tag::kOrigClOrdId), "a1");
    EXPECT_EQ(field(m, tag::kOrderQty), "50");
    EXPECT_EQ(field(m, tag::kLeavesQty), "50");

    // The old ClOrdID is retired; the new one is live.
    replies = send(alice.build(fix::msg::kOrderCancelRequest, {{tag::kClOrdId, "c1"},
                                                               {tag::kOrigClOrdId, "a1"},
                                                               {tag::kSymbol, "AAPL"},
                                                               {tag::kSide, "1"}}));
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kOrderCancelReject);
    replies = send(alice.build(fix::msg::kOrderCancelRequest, {{tag::kClOrdId, "c2"},
                                                               {tag::kOrigClOrdId, "a2"},
                                                               {tag::kSymbol, "AAPL"},
                                                               {tag::kSide, "1"}}));
    EXPECT_EQ(field(replies[0].second, tag::kExecType), "4");
}

TEST_F(GatewayTest, ReplaceIntoCrossFills) {
    logon(alice);
    logon(bob);
    new_order(alice, "a1", "2", "100", "10.10");
    new_order(bob, "b1", "1", "100", "10.00");
    const auto replies =
        send(bob.build(fix::msg::kOrderCancelReplaceRequest, {{tag::kClOrdId, "b2"},
                                                              {tag::kOrigClOrdId, "b1"},
                                                              {tag::kSymbol, "AAPL"},
                                                              {tag::kSide, "1"},
                                                              {tag::kOrderQty, "100"},
                                                              {tag::kOrdType, "2"},
                                                              {tag::kPrice, "10.10"}}));
    ASSERT_EQ(replies.size(), 3U);
    EXPECT_EQ(field(replies[0].second, tag::kExecType), "5");
    EXPECT_EQ(field(replies[1].second, tag::kExecType), "F");
    EXPECT_EQ(field(replies[1].second, tag::kClOrdId), "b2");
    EXPECT_EQ(replies[2].first, "ALICE");
    EXPECT_EQ(gateway.open_orders(), 0U);
}

TEST_F(GatewayTest, ReplaceCannotChangeSide) {
    logon(alice);
    new_order(alice, "a1", "1", "100", "10.00");
    const auto replies =
        send(alice.build(fix::msg::kOrderCancelReplaceRequest, {{tag::kClOrdId, "a2"},
                                                                {tag::kOrigClOrdId, "a1"},
                                                                {tag::kSymbol, "AAPL"},
                                                                {tag::kSide, "2"},
                                                                {tag::kOrderQty, "100"},
                                                                {tag::kOrdType, "2"},
                                                                {tag::kPrice, "10.00"}}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kReject);
}

TEST_F(GatewayTest, EngineRejectionBecomesRejectedExecutionReport) {
    logon(alice);
    const auto replies = new_order(alice, "a1", "1", "100", "10.001");
    ASSERT_EQ(replies.size(), 1U);
    const Message& m = replies[0].second;
    EXPECT_EQ(field(m, tag::kExecType), "8");
    EXPECT_EQ(field(m, tag::kOrdStatus), "8");
    EXPECT_EQ(field(m, tag::kOrderId), "NONE");
    EXPECT_EQ(field(m, tag::kText), "price not on tick");
}

TEST_F(GatewayTest, UnknownSymbolIsRejected) {
    logon(alice);
    const auto replies = send(alice.build(fix::msg::kNewOrderSingle, {{tag::kClOrdId, "a1"},
                                                                      {tag::kSymbol, "ZZZZ"},
                                                                      {tag::kSide, "1"},
                                                                      {tag::kOrderQty, "1"},
                                                                      {tag::kOrdType, "2"},
                                                                      {tag::kPrice, "1"}}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(field(replies[0].second, tag::kOrdRejReason), "1");
}

TEST_F(GatewayTest, DuplicateLiveClOrdIdIsRejected) {
    logon(alice);
    new_order(alice, "dup", "1", "100", "10.00");
    const auto replies = new_order(alice, "dup", "1", "100", "10.00");
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(field(replies[0].second, tag::kExecType), "8");
    EXPECT_EQ(field(replies[0].second, tag::kText), "duplicate ClOrdID");
    EXPECT_EQ(gateway.open_orders(), 1U);
}

TEST_F(GatewayTest, MissingRequiredTagIsSessionRejected) {
    logon(alice);
    const auto replies =
        send(alice.build(fix::msg::kNewOrderSingle,
                         {{tag::kClOrdId, "a1"}, {tag::kSide, "1"}, {tag::kOrderQty, "1"}}));
    ASSERT_EQ(replies.size(), 1U);
    const Message& m = replies[0].second;
    EXPECT_EQ(m.msg_type(), fix::msg::kReject);
    EXPECT_EQ(field(m, tag::kRefTagId), "55");
    EXPECT_EQ(field(m, tag::kSessionRejectReason), "1");
    EXPECT_EQ(field(m, tag::kRefSeqNum), "2");
}

TEST_F(GatewayTest, BadFieldValuesAreSessionRejected) {
    logon(alice);
    auto replies = new_order(alice, "a1", "7", "100", "10.00");
    EXPECT_EQ(field(replies[0].second, tag::kRefTagId), "54");
    replies = new_order(alice, "a1", "1", "1.5", "10.00");
    EXPECT_EQ(field(replies[0].second, tag::kRefTagId), "38");
    replies = new_order(alice, "a1", "1", "100", "abc");
    EXPECT_EQ(field(replies[0].second, tag::kRefTagId), "44");
    replies = new_order(alice, "a1", "1", "100", "10.00", {{tag::kTimeInForce, "9"}});
    EXPECT_EQ(field(replies[0].second, tag::kRefTagId), "59");
    EXPECT_TRUE(events.accepted.empty());
}

TEST_F(GatewayTest, ImmediateOrCancelRemainderIsReported) {
    logon(alice);
    logon(bob);
    new_order(alice, "a1", "2", "10", "10.00");
    const auto replies = new_order(bob, "b1", "1", "25", "10.00", {{tag::kTimeInForce, "3"}});
    ASSERT_EQ(replies.size(), 4U);  // ack, fill, alice fill, cancel of remainder
    const Message& cancel = replies[3].second;
    EXPECT_EQ(field(cancel, tag::kExecType), "4");
    EXPECT_EQ(field(cancel, tag::kCumQty), "10");
    EXPECT_EQ(field(cancel, tag::kText), "IOC remainder");
}

TEST_F(GatewayTest, MarketOrderDefaultsToImmediateOrCancel) {
    logon(alice);
    logon(bob);
    new_order(alice, "a1", "2", "10", "10.00");
    const auto replies = new_order(bob, "b1", "1", "15", "");
    ASSERT_EQ(replies.size(), 4U);
    EXPECT_EQ(field(replies[3].second, tag::kText), "market order remainder");
    EXPECT_EQ(field(replies[0].second, tag::kOrdType), "1");
}

TEST_F(GatewayTest, PostOnlyIcebergAndSelfMatchFlagsReachTheEngine) {
    logon(alice);
    new_order(alice, "a1", "2", "1000", "10.00",
              {{tag::kDisplayQty, "100"}, {tag::kExecInst, "6"}});
    const Order* order = engine.book(1)->find(1);
    ASSERT_NE(order, nullptr);
    EXPECT_TRUE(order->post_only);
    EXPECT_EQ(order->display, 100);

    // Self-match prevention: Alice crossing her own order with cancel-aggressive.
    const auto replies =
        new_order(alice, "a2", "1", "50", "10.00", {{tag::kSelfMatchPrevention, "1"}});
    ASSERT_EQ(replies.size(), 2U);
    EXPECT_EQ(field(replies[1].second, tag::kExecType), "4");
    EXPECT_EQ(field(replies[1].second, tag::kText), "self-trade prevention");
    EXPECT_TRUE(events.trades.empty());
}

TEST_F(GatewayTest, TestRequestIsAnsweredWithHeartbeat) {
    logon(alice);
    const auto replies = send(alice.build(fix::msg::kTestRequest, {{tag::kTestReqId, "ping"}}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kHeartbeat);
    EXPECT_EQ(field(replies[0].second, tag::kTestReqId), "ping");
    EXPECT_TRUE(send(alice.build(fix::msg::kHeartbeat, {})).empty());
}

TEST_F(GatewayTest, SequenceGapEndsSession) {
    logon(alice);
    alice.skip_sequence();
    const auto replies = send(alice.build(fix::msg::kHeartbeat, {}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kLogout);
    EXPECT_EQ(field(replies[0].second, tag::kText), "MsgSeqNum 3 but expected 2");
    EXPECT_FALSE(gateway.logged_on("ALICE"));
}

TEST_F(GatewayTest, LogoutAndLogBackOnResetsSequences) {
    logon(alice);
    auto replies = send(alice.build(fix::msg::kLogout, {}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kLogout);
    EXPECT_FALSE(gateway.logged_on("ALICE"));

    Client again{"ALICE"};
    logon(again);
    replies = send(again.build(fix::msg::kTestRequest, {{tag::kTestReqId, "x"}}));
    EXPECT_EQ(field(replies[0].second, tag::kMsgSeqNum), "2");
}

TEST_F(GatewayTest, OrdersSurviveLogoutAndCanBeCancelledLater) {
    logon(alice);
    new_order(alice, "a1", "1", "100", "10.00");
    send(alice.build(fix::msg::kLogout, {}));
    Client again{"ALICE"};
    logon(again);
    const auto replies = send(again.build(fix::msg::kOrderCancelRequest, {{tag::kClOrdId, "c1"},
                                                                          {tag::kOrigClOrdId, "a1"},
                                                                          {tag::kSymbol, "AAPL"},
                                                                          {tag::kSide, "1"}}));
    EXPECT_EQ(field(replies[0].second, tag::kExecType), "4");
}

TEST_F(GatewayTest, GarbledMessageOnLiveSessionGetsRejectWithoutConsumingSequence) {
    logon(alice);
    std::string raw = alice.build(fix::msg::kHeartbeat, {});
    raw[raw.size() - 3] = raw[raw.size() - 3] == '9' ? '8' : '9';  // corrupt checksum
    auto replies = send(raw);
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kReject);
    EXPECT_EQ(field(replies[0].second, tag::kText), "CheckSum does not match the message");
    // The garbled message's sequence number (2) is still expected.
    replies = send(alice.build_with_seq(fix::msg::kTestRequest, {{tag::kTestReqId, "t"}}, 2));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kHeartbeat);
}

TEST_F(GatewayTest, UnsupportedMsgTypeIsRejected) {
    logon(alice);
    const auto replies = send(alice.build("Z", {}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(field(replies[0].second, tag::kSessionRejectReason), "11");
}

TEST_F(GatewayTest, WrongTargetCompIdIsRefused) {
    Builder builder(fix::msg::kLogon);
    builder.add(tag::kSenderCompId, std::string_view("EVE"))
        .add(tag::kTargetCompId, std::string_view("SOMEONE"))
        .add(tag::kMsgSeqNum, std::int64_t{1});
    const auto replies = send(builder.finish());
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].second.msg_type(), fix::msg::kLogout);
    EXPECT_FALSE(gateway.logged_on("EVE"));
}

TEST_F(GatewayTest, FillsForOrdersTradedOutsideHandleAreQueued) {
    logon(alice);
    new_order(alice, "a1", "2", "10", "10.00");
    NewOrderRequest direct;
    direct.instrument = 1;
    direct.side = Side::Buy;
    direct.price = 100'000;
    direct.quantity = 10;
    engine.submit(direct);
    const auto queued = gateway.drain();
    ASSERT_EQ(queued.size(), 1U);
    EXPECT_EQ(queued[0].session, "ALICE");
    EXPECT_NE(queued[0].text.find("150=F"), std::string::npos);
    EXPECT_TRUE(gateway.drain().empty());
}

}  // namespace
}  // namespace quill::test
