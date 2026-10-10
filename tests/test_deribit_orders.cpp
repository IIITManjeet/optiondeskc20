#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "od/deribit_orders.hpp"

using nlohmann::json;
using od::ExecEvent;
using od::deribit::DeribitOrders;

namespace {

od::OrderRequest bid(std::uint64_t id, double price = 0.0215, double qty = 0.5) {
    return {id, "BTC-27NOV26-88000-C", od::Side::Buy, price, qty, true};
}

int rpc_id(const std::string& text) { return json::parse(text).at("id").get<int>(); }

json response(int id, json result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }
json error(int id, const char* message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", 11054}, {"message", message}}}};
}
json order(const std::string& label, const char* state, double price = 0.0215, const char* oid = "OID-1") {
    return {{"order_id", oid}, {"label", label}, {"order_state", state}, {"price", price}, {"amount", 0.5}};
}
json notify(const char* channel, json data) {
    return {{"jsonrpc", "2.0"}, {"method", "subscription"}, {"params", {{"channel", channel}, {"data", std::move(data)}}}};
}

struct Harness {
    DeribitOrders d{"run7"};
    std::vector<ExecEvent> ev;
    std::vector<std::string> out;
    void feed(const json& m) { d.on_message(m, ev, out); }
};

}  // namespace

TEST(DeribitOrders, PlaceBuildsPostOnlyLimitWithRunLabel) {
    DeribitOrders d("run7");
    const auto msg = json::parse(*d.place(bid(3)));
    EXPECT_EQ(msg["method"], "private/buy");
    const auto& p = msg["params"];
    EXPECT_EQ(p["instrument_name"], "BTC-27NOV26-88000-C");
    EXPECT_EQ(p["type"], "limit");
    EXPECT_DOUBLE_EQ(p["price"].get<double>(), 0.0215);
    EXPECT_DOUBLE_EQ(p["amount"].get<double>(), 0.5);
    EXPECT_EQ(p["post_only"], true);
    EXPECT_EQ(p["reject_post_only"], true);
    EXPECT_EQ(p["label"], "run7-3");
}

TEST(DeribitOrders, PerpHedgeIsAMarketOrderInUsd) {
    DeribitOrders d("run7");
    const auto msg = json::parse(*d.place({4, "BTC-PERPETUAL", od::Side::Sell, 81000, 2500, false}));
    EXPECT_EQ(msg["method"], "private/sell");
    EXPECT_EQ(msg["params"]["type"], "market");
    EXPECT_FALSE(msg["params"].contains("price"));
    EXPECT_DOUBLE_EQ(msg["params"]["amount"].get<double>(), 2500);
}

TEST(DeribitOrders, AckThenCancelFromBothResponseAndNotificationReportsOnce) {
    Harness h;
    const int place_id = rpc_id(*h.d.place(bid(1)));
    h.feed(response(place_id, {{"order", order("run7-1", "open")}, {"trades", json::array()}}));
    ASSERT_EQ(h.ev.size(), 1u);
    EXPECT_EQ(h.ev[0].kind, ExecEvent::Kind::Ack);

    const auto cancel_msg = json::parse(*h.d.cancel(1));
    EXPECT_EQ(cancel_msg["method"], "private/cancel");
    EXPECT_EQ(cancel_msg["params"]["order_id"], "OID-1");
    h.feed(response(cancel_msg["id"].get<int>(), order("run7-1", "cancelled")));
    h.feed(notify("user.orders.any.BTC.raw", order("run7-1", "cancelled")));
    ASSERT_EQ(h.ev.size(), 2u);
    EXPECT_EQ(h.ev[1].kind, ExecEvent::Kind::Cancelled);
    EXPECT_EQ(h.d.live_orders(), 0u);
}

TEST(DeribitOrders, AmendAndCancelBeforeAckAreHeldThenSent) {
    Harness h;
    const int place_id = rpc_id(*h.d.place(bid(2)));
    EXPECT_FALSE(h.d.amend(2, 0.0220, 0.5));  // no order_id yet: held
    h.feed(response(place_id, {{"order", order("run7-2", "open", 0.0215, "OID-9")}}));
    ASSERT_EQ(h.out.size(), 1u);
    const auto amend = json::parse(h.out[0]);
    EXPECT_EQ(amend["method"], "private/edit");
    EXPECT_EQ(amend["params"]["order_id"], "OID-9");
    EXPECT_DOUBLE_EQ(amend["params"]["price"].get<double>(), 0.0220);

    // A cancel held behind an amend wins: the amend is dropped.
    Harness g;
    const int pid = rpc_id(*g.d.place(bid(5)));
    g.d.amend(5, 0.0220, 0.5);
    EXPECT_FALSE(g.d.cancel(5));
    g.feed(response(pid, {{"order", order("run7-5", "open", 0.0215, "OID-5")}}));
    ASSERT_EQ(g.out.size(), 1u);
    EXPECT_EQ(json::parse(g.out[0])["method"], "private/cancel");
}

TEST(DeribitOrders, RejectedPlaceAndFailedAmend) {
    Harness h;
    const int place_id = rpc_id(*h.d.place(bid(1)));
    h.feed(error(place_id, "post_only_reject"));
    ASSERT_EQ(h.ev.size(), 1u);
    EXPECT_EQ(h.ev[0].kind, ExecEvent::Kind::Rejected);
    EXPECT_EQ(h.ev[0].reason, "post_only_reject");

    Harness g;
    const int pid = rpc_id(*g.d.place(bid(2, 0.0215)));
    g.feed(response(pid, {{"order", order("run7-2", "open")}}));
    const int aid = rpc_id(*g.d.amend(2, 0.0230, 0.5));
    g.feed(error(aid, "post_only_reject"));
    ASSERT_EQ(g.ev.size(), 2u);
    EXPECT_EQ(g.ev[1].kind, ExecEvent::Kind::Ack);
    EXPECT_DOUBLE_EQ(g.ev[1].price, 0.0215);  // still working at the old price
}

TEST(DeribitOrders, FillsComeFromTradesAndAreDeduplicated) {
    Harness h;
    const int pid = rpc_id(*h.d.place(bid(1)));
    const json trade{{"trade_id", "T-1"}, {"label", "run7-1"}, {"order_id", "OID-1"},
                     {"amount", 0.2}, {"price", 0.0215}, {"fee", 0.00006}};
    h.feed(response(pid, {{"order", order("run7-1", "open")}, {"trades", json::array({trade})}}));
    h.feed(notify("user.trades.any.BTC.raw", json::array({trade})));  // same trade again
    std::size_t fills = 0;
    for (const auto& e : h.ev) {
        if (e.kind != ExecEvent::Kind::Fill) continue;
        ++fills;
        EXPECT_DOUBLE_EQ(e.qty, 0.2);
        EXPECT_DOUBLE_EQ(e.fee, 0.00006);
    }
    EXPECT_EQ(fills, 1u);

    // Amending a partly filled order sends total size = filled + new remaining.
    const auto amend = json::parse(*h.d.amend(1, 0.0210, 0.5));
    EXPECT_DOUBLE_EQ(amend["params"]["amount"].get<double>(), 0.7);
}

TEST(DeribitOrders, IgnoresOrdersFromOtherRuns) {
    Harness h;
    h.d.place(bid(1));
    h.feed(notify("user.orders.any.BTC.raw", order("run6-1", "cancelled")));
    h.feed(notify("user.orders.any.BTC.raw", order("manual", "cancelled")));
    h.feed(notify("user.trades.any.BTC.raw",
                  json::array({{{"trade_id", "X"}, {"label", "run6-1"}, {"amount", 1.0}, {"price", 0.01}}})));
    EXPECT_TRUE(h.ev.empty());
}

TEST(DeribitOrders, DisconnectCancelsEverythingLive) {
    Harness h;
    const int a = rpc_id(*h.d.place(bid(1)));
    h.d.place(bid(2));
    h.feed(response(a, {{"order", order("run7-1", "open")}}));
    h.ev.clear();
    h.d.on_disconnect(h.ev);
    EXPECT_EQ(h.ev.size(), 2u);
    for (const auto& e : h.ev) EXPECT_EQ(e.kind, ExecEvent::Kind::Cancelled);
    EXPECT_EQ(h.d.live_orders(), 0u);
}
