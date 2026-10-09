#include <gtest/gtest.h>

#include <cmath>

#include "od/gateway.hpp"
#include "od/quoter.hpp"
#include "od/risk_gate.hpp"

// --- Tick ladder ----------------------------------------------------------------

TEST(Tick, DeribitLadder) {
    EXPECT_DOUBLE_EQ(od::option_tick(0.0042), 0.0001);
    EXPECT_DOUBLE_EQ(od::option_tick(0.005), 0.0005);
    EXPECT_NEAR(od::round_to_tick(0.02173, true), 0.0215, 1e-12);
    EXPECT_NEAR(od::round_to_tick(0.02173, false), 0.0220, 1e-12);
    EXPECT_NEAR(od::round_to_tick(0.00437, true), 0.0043, 1e-12);
    EXPECT_NEAR(od::round_to_tick(0.0215, true), 0.0215, 1e-12);  // already on the grid
}

// --- Quoter ---------------------------------------------------------------------

namespace {
od::QuoteInputs base_inputs() {
    od::QuoteInputs in;
    in.type = od::OptionType::Call;
    in.strike = 88000;
    in.forward = 82000;
    in.T = 0.13;
    in.theo_iv = 0.37;
    return in;
}
}  // namespace

TEST(Quoter, SymmetricAroundTheoInVol) {
    const auto q = od::make_quote(base_inputs(), {});
    ASSERT_TRUE(q.bid && q.ask);
    EXPECT_LT(*q.bid, q.theo_coin);
    EXPECT_GT(*q.ask, q.theo_coin);
    EXPECT_NEAR(q.bid_iv, 0.355, 1e-12);
    EXPECT_NEAR(q.ask_iv, 0.385, 1e-12);
    EXPECT_NEAR(*q.bid, od::round_to_tick(*q.bid, true), 1e-12);
    EXPECT_NEAR(*q.ask, od::round_to_tick(*q.ask, false), 1e-12);
}

TEST(Quoter, LongVegaSkewsBothQuotesDown) {
    auto in = base_inputs();
    const auto flat = od::make_quote(in, {});
    in.portfolio_vega_usd = 2000;  // long vega -> want to sell
    const auto skewed = od::make_quote(in, {});
    EXPECT_LT(skewed.skew_vol, 0.0);
    EXPECT_LT(skewed.bid_iv, flat.bid_iv);
    EXPECT_LT(skewed.ask_iv, flat.ask_iv);
    EXPECT_LE(*skewed.ask, *flat.ask);
}

TEST(Quoter, SkewIsCapped) {
    auto in = base_inputs();
    in.portfolio_vega_usd = -1e9;
    od::QuoteParams p;
    EXPECT_DOUBLE_EQ(od::make_quote(in, p).skew_vol, p.max_skew_vol);
}

TEST(Quoter, NeverCrossesTheBook) {
    auto in = base_inputs();
    const auto free_q = od::make_quote(in, {});
    in.best_ask = *free_q.bid;   // market offer sits at our would-be bid
    in.best_bid = *free_q.ask;   // and the market bid at our would-be offer
    const auto q = od::make_quote(in, {});
    if (q.bid) {
        EXPECT_LT(*q.bid, in.best_ask);
    }
    if (q.ask) {
        EXPECT_GT(*q.ask, in.best_bid);
    }
    if (q.bid && q.ask) {
        EXPECT_LT(*q.bid, *q.ask);
    }
}

TEST(Quoter, PositionLimitDropsOneSide) {
    auto in = base_inputs();
    od::QuoteParams p;
    in.position = p.max_position;  // full long: no more bids
    auto q = od::make_quote(in, p);
    EXPECT_FALSE(q.bid);
    EXPECT_TRUE(q.ask);
    in.position = -p.max_position;
    q = od::make_quote(in, p);
    EXPECT_TRUE(q.bid);
    EXPECT_FALSE(q.ask);
}

// --- Risk gate ------------------------------------------------------------------

namespace {
od::OrderRequest opt_order(od::Side side, double qty, double price = 0.02) {
    return {1, "BTC-27NOV26-88000-C", side, price, qty, true};
}
od::GateContext ok_ctx() {
    od::GateContext c;
    c.vega_per_contract_usd = 100;
    c.theo_iv = 0.37;
    c.order_iv = 0.37;
    return c;
}
constexpr std::int64_t kT0 = 1'000'000'000;
}  // namespace

TEST(RiskGate, AcceptsNormalOrder) {
    od::RiskGate g({});
    EXPECT_FALSE(g.check(opt_order(od::Side::Buy, 0.5), ok_ctx(), kT0));
}

TEST(RiskGate, OrderSize) {
    od::RiskGate g({});
    EXPECT_EQ(g.check(opt_order(od::Side::Buy, 5), ok_ctx(), kT0), "max_order_qty");
}

TEST(RiskGate, WorstCasePositionCountsOpenOrders) {
    od::RiskGate g({});  // max_position 3
    auto ctx = ok_ctx();
    ctx.position = 2.0;
    ctx.open_same_side = 0.5;
    EXPECT_EQ(g.check(opt_order(od::Side::Buy, 1.0), ctx, kT0), "max_position");
    EXPECT_FALSE(g.check(opt_order(od::Side::Sell, 1.0), ctx, kT0));  // reduces: allowed
}

TEST(RiskGate, VegaLimitAllowsRiskReducingOrders) {
    od::RiskGate g({});  // max 3000
    auto ctx = ok_ctx();
    ctx.portfolio_vega_usd = 2990;
    EXPECT_EQ(g.check(opt_order(od::Side::Buy, 0.5), ctx, kT0), "max_vega");
    EXPECT_FALSE(g.check(opt_order(od::Side::Sell, 0.5), ctx, kT0));
}

TEST(RiskGate, FatFingerPriceBand) {
    od::RiskGate g({});
    auto ctx = ok_ctx();
    ctx.order_iv = 0.60;  // 23 vol points away from theo
    EXPECT_EQ(g.check(opt_order(od::Side::Buy, 0.5), ctx, kT0), "price_band");
    ctx.order_iv = 0.0;
    EXPECT_EQ(g.check(opt_order(od::Side::Buy, 0.5), ctx, kT0), "price_unpriceable");
}

TEST(RiskGate, TokenBucketRateLimit) {
    od::RiskLimits lim;
    lim.max_orders_per_sec = 10;
    lim.burst = 3;
    od::RiskGate g(lim);
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(g.check(opt_order(od::Side::Buy, 0.1), ok_ctx(), kT0));
    EXPECT_EQ(g.check(opt_order(od::Side::Buy, 0.1), ok_ctx(), kT0), "rate_limit");
    // 100 ms later one token has refilled.
    EXPECT_FALSE(g.check(opt_order(od::Side::Buy, 0.1), ok_ctx(), kT0 + 100'000'000));
}

TEST(RiskGate, KillSwitchBlocksEverything) {
    od::RiskGate g({});
    g.kill();
    EXPECT_EQ(g.check(opt_order(od::Side::Sell, 0.1), ok_ctx(), kT0), "kill_switch");
}

// --- Order manager / ledger -------------------------------------------------------

TEST(OrderManager, OptionPremiumAndFeesFlowThroughCash) {
    od::OrderManager om("BTC", 1.0);
    od::OrderRequest buy{om.next_id(), "C", od::Side::Buy, 0.02, 2.0, true};
    om.on_sent(buy);
    om.apply(od::ExecEvent::ack(buy.client_id));
    EXPECT_EQ(om.find(buy.client_id)->state, od::OrderState::Open);
    om.apply(od::ExecEvent::fill(buy.client_id, 2.0, 0.02, 0.0006));
    EXPECT_EQ(om.find(buy.client_id)->state, od::OrderState::Filled);
    EXPECT_DOUBLE_EQ(om.position("C"), 2.0);
    EXPECT_NEAR(om.coin_balance(), 1.0 - 0.04 - 0.0006, 1e-15);

    od::OrderRequest sell{om.next_id(), "C", od::Side::Sell, 0.025, 2.0, true};
    om.on_sent(sell);
    om.apply(od::ExecEvent::fill(sell.client_id, 2.0, 0.025, 0.0006));
    EXPECT_DOUBLE_EQ(om.position("C"), 0.0);
    // Round trip: +0.01 premium P&L, -0.0012 fees.
    EXPECT_NEAR(om.coin_balance(), 1.0 + 0.01 - 0.0012, 1e-15);
    EXPECT_TRUE(om.portfolio().positions.empty());
}

TEST(OrderManager, InversePerpEntryAndRealisedPnl) {
    od::OrderManager om("BTC", 0.0);
    auto fill = [&](od::Side side, double usd, double px) {
        od::OrderRequest r{om.next_id(), "BTC-PERPETUAL", side, px, usd, false};
        om.on_sent(r);
        om.apply(od::ExecEvent::fill(r.client_id, usd, px, 0.0));
    };
    fill(od::Side::Buy, 10000, 80000);
    fill(od::Side::Buy, 10000, 100000);
    // Harmonic entry: 20000 / (10000/80000 + 10000/100000)
    const auto pf = om.portfolio();
    ASSERT_EQ(pf.positions.size(), 1u);
    EXPECT_NEAR(pf.positions[0].entry_price, 20000.0 / (0.125 + 0.1), 1e-6);
    // Close everything at 90000: realised = 20000 * (1/entry - 1/90000)
    const double entry = pf.positions[0].entry_price;
    fill(od::Side::Sell, 20000, 90000);
    EXPECT_NEAR(om.coin_balance(), 20000 * (1 / entry - 1 / 90000.0), 1e-12);
    EXPECT_DOUBLE_EQ(om.position("BTC-PERPETUAL"), 0.0);
}

TEST(OrderManager, OpenQtyTracksLiveOrdersOnly) {
    od::OrderManager om("BTC", 1.0);
    od::OrderRequest a{om.next_id(), "C", od::Side::Buy, 0.02, 0.5, true};
    od::OrderRequest b{om.next_id(), "C", od::Side::Buy, 0.019, 0.5, true};
    om.on_sent(a);
    om.on_sent(b);
    EXPECT_DOUBLE_EQ(om.open_qty("C", od::Side::Buy), 1.0);
    om.apply(od::ExecEvent::cancelled(a.client_id));
    EXPECT_DOUBLE_EQ(om.open_qty("C", od::Side::Buy), 0.5);
    EXPECT_DOUBLE_EQ(om.open_qty("C", od::Side::Sell), 0.0);
}

// --- Simulated exchange -----------------------------------------------------------

TEST(SimGateway, PostOnlyRejectsCrossingOrders) {
    od::SimGateway gw;
    gw.on_market("C", 0.020, 0.022);
    gw.place({1, "C", od::Side::Buy, 0.022, 1, true});
    std::vector<od::ExecEvent> ev;
    gw.poll(ev);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, od::ExecEvent::Kind::Rejected);
    EXPECT_EQ(ev[0].reason, "post_only_would_cross");
}

TEST(SimGateway, FillsOnlyWhenMarketTradesThrough) {
    od::SimGateway gw;
    gw.on_market("C", 0.020, 0.022);
    gw.place({1, "C", od::Side::Buy, 0.0205, 1, true});
    gw.place({2, "C", od::Side::Sell, 0.0215, 1, true});
    std::vector<od::ExecEvent> ev;
    gw.poll(ev);
    ASSERT_EQ(ev.size(), 2u);  // two acks

    gw.on_market("C", 0.0202, 0.0210);  // inside our quotes: nothing
    ev.clear();
    gw.poll(ev);
    EXPECT_TRUE(ev.empty());

    gw.on_market("C", 0.0198, 0.0205);  // offer down to our bid
    ev.clear();
    gw.poll(ev);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, od::ExecEvent::Kind::Fill);
    EXPECT_EQ(ev[0].client_id, 1u);
    EXPECT_DOUBLE_EQ(ev[0].price, 0.0205);
    EXPECT_NEAR(ev[0].fee, 0.0003, 1e-15);
    EXPECT_EQ(gw.resting(), 1u);
}

TEST(SimGateway, AmendThatWouldCrossKeepsOldPrice) {
    od::SimGateway gw;
    gw.on_market("C", 0.020, 0.022);
    gw.place({1, "C", od::Side::Buy, 0.0205, 1, true});
    gw.amend(1, 0.023, 1);
    std::vector<od::ExecEvent> ev;
    gw.poll(ev);
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_DOUBLE_EQ(ev[1].price, 0.0205);  // ack reports the price still working

    od::OrderManager om("BTC", 1);
    om.on_sent({1, "C", od::Side::Buy, 0.0205, 1, true});
    om.on_amend_sent(1, 0.023, 1);
    for (const auto& e : ev) om.apply(e);
    EXPECT_DOUBLE_EQ(om.find(1)->req.price, 0.0205);
}

TEST(SimGateway, CancelAll) {
    od::SimGateway gw;
    gw.place({1, "C", od::Side::Buy, 0.01, 1, true});
    gw.place({2, "P", od::Side::Sell, 0.03, 1, true});
    gw.cancel_all();
    EXPECT_EQ(gw.resting(), 0u);
    std::vector<od::ExecEvent> ev;
    gw.poll(ev);
    EXPECT_EQ(ev.size(), 4u);
}

// --- Hedger -----------------------------------------------------------------------

TEST(Hedge, ThresholdAndContractRounding) {
    od::HedgeParams p;  // 0.25 coin, $10 contracts
    EXPECT_EQ(od::hedge_notional_usd(0.2, 80000, p), 0.0);
    EXPECT_EQ(od::hedge_notional_usd(0.30004, 80000, p), -24000.0);  // 24003.2 -> 24000
    EXPECT_EQ(od::hedge_notional_usd(-1.0, 81234, p), 81230.0);
}

// --- Queue-position fill model ------------------------------------------------------

namespace {
std::vector<od::ExecEvent> drain(od::SimGateway& gw) {
    std::vector<od::ExecEvent> ev;
    gw.poll(ev);
    return ev;
}
double filled(const std::vector<od::ExecEvent>& ev) {
    double q = 0;
    for (const auto& e : ev)
        if (e.kind == od::ExecEvent::Kind::Fill) q += e.qty;
    return q;
}
}  // namespace

TEST(SimQueue, JoiningTheBidQueuesBehindDisplayedSize) {
    od::SimGateway gw;
    gw.on_market("C", 0.0200, 0.0210, 5.0, 3.0);
    gw.place({1, "C", od::Side::Buy, 0.0200, 1.0, true});
    drain(gw);
    EXPECT_DOUBLE_EQ(gw.queue_ahead(1), 5.0);

    gw.on_trade("C", 0.0200, 3.0, /*taker_buy=*/false);  // sellers hit the bid
    EXPECT_DOUBLE_EQ(filled(drain(gw)), 0.0);
    EXPECT_DOUBLE_EQ(gw.queue_ahead(1), 2.0);

    gw.on_trade("C", 0.0200, 2.5, false);  // 2 ahead of us, 0.5 reaches us
    EXPECT_DOUBLE_EQ(filled(drain(gw)), 0.5);
    EXPECT_EQ(gw.resting(), 1u);  // partially filled, still working

    gw.on_trade("C", 0.0200, 10.0, false);
    EXPECT_DOUBLE_EQ(filled(drain(gw)), 0.5);
    EXPECT_EQ(gw.resting(), 0u);
}

TEST(SimQueue, ImprovingThePriceIsFirstInQueue) {
    od::SimGateway gw;
    gw.on_market("C", 0.0200, 0.0215, 5.0, 3.0);
    gw.place({1, "C", od::Side::Buy, 0.0205, 1.0, true});
    drain(gw);
    EXPECT_DOUBLE_EQ(gw.queue_ahead(1), 0.0);
    gw.on_trade("C", 0.0205, 0.3, false);
    EXPECT_DOUBLE_EQ(filled(drain(gw)), 0.3);
}

TEST(SimQueue, TradesOnTheOtherSideDontFillUs) {
    od::SimGateway gw;
    gw.on_market("C", 0.0200, 0.0210, 0.0, 0.0);
    gw.place({1, "C", od::Side::Buy, 0.0200, 1.0, true});
    drain(gw);
    gw.on_trade("C", 0.0210, 5.0, /*taker_buy=*/true);  // buyers lifting offers
    EXPECT_DOUBLE_EQ(filled(drain(gw)), 0.0);
}

TEST(SimQueue, TradeThroughOurPriceFillsFully) {
    od::SimGateway gw;
    gw.on_market("C", 0.0200, 0.0210, 50.0, 3.0);
    gw.place({1, "C", od::Side::Sell, 0.0210, 1.0, true});
    drain(gw);
    EXPECT_DOUBLE_EQ(gw.queue_ahead(1), 3.0);
    gw.on_trade("C", 0.0215, 0.1, /*taker_buy=*/true);  // printed above our offer
    EXPECT_DOUBLE_EQ(filled(drain(gw)), 1.0);
}

TEST(SimQueue, CancellationsAheadMoveUsUp) {
    od::SimGateway gw;
    gw.on_market("C", 0.0200, 0.0210, 5.0, 3.0);
    gw.place({1, "C", od::Side::Buy, 0.0200, 1.0, true});
    drain(gw);
    gw.on_market("C", 0.0200, 0.0210, 2.0, 3.0);  // displayed bid size dropped to 2
    EXPECT_DOUBLE_EQ(gw.queue_ahead(1), 2.0);
    gw.on_market("C", 0.0200, 0.0210, 4.0, 3.0);  // size added later queues behind us
    EXPECT_DOUBLE_EQ(gw.queue_ahead(1), 2.0);
}

TEST(SimQueue, BehindTheBestThenBestAgainAssumesLast) {
    od::SimGateway gw;
    gw.on_market("C", 0.0200, 0.0210, 5.0, 3.0);
    gw.place({1, "C", od::Side::Buy, 0.0195, 1.0, true});
    drain(gw);
    EXPECT_TRUE(std::isinf(gw.queue_ahead(1)));
    gw.on_trade("C", 0.0195, 100.0, false);  // place unknown: can't claim a fill...
    EXPECT_DOUBLE_EQ(filled(drain(gw)), 0.0);
    gw.on_market("C", 0.0195, 0.0205, 7.0, 3.0);  // ...until our level is the best
    EXPECT_DOUBLE_EQ(gw.queue_ahead(1), 7.0);
}

TEST(SimQueue, RepricingLosesPriority) {
    od::SimGateway gw;
    gw.on_market("C", 0.0200, 0.0215, 5.0, 3.0);
    gw.place({1, "C", od::Side::Buy, 0.0205, 1.0, true});  // first in queue
    drain(gw);
    gw.amend(1, 0.0200, 1.0);  // move down to join the bid: back of the queue
    drain(gw);
    EXPECT_DOUBLE_EQ(gw.queue_ahead(1), 5.0);
}
