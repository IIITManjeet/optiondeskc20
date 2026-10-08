#include <gtest/gtest.h>

#include <cmath>

#include "od/risk.hpp"

namespace {

constexpr double kF = 100000.0;
constexpr double kT = 0.25;
constexpr std::int64_t kExpiry = 1'000'000'000'000;

od::OptionQuote make_quote(const std::string& name, od::OptionType type, double strike, double iv) {
    od::OptionQuote q;
    q.name = name;
    q.type = type;
    q.strike = strike;
    q.expiry_ms = kExpiry;
    q.mark_iv = iv;
    q.underlying = kF;
    return q;
}

// One expiry, forward == index so the USD and coin views are easy to reason about.
od::OptionChain make_chain(double index = kF) {
    od::OptionChain c;
    c.currency = "BTC";
    c.index_price = index;
    od::ExpirySlice s;
    s.expiry_ms = kExpiry;
    s.label = "TEST";
    s.T = kT;
    s.forward = kF;
    s.quotes = {make_quote("C100", od::OptionType::Call, 100000, 0.5),
                make_quote("P90", od::OptionType::Put, 90000, 0.6),
                make_quote("C120", od::OptionType::Call, 120000, 0.45)};
    c.expiries.push_back(s);
    return c;
}

od::Portfolio book(std::vector<od::Position> positions, double balance = 0.0) {
    return {"BTC", balance, std::move(positions)};
}

od::Position opt(const char* name, double qty) { return {name, od::PositionKind::Option, qty, 0.0}; }
od::Position perp(double usd, double entry) {
    return {"BTC-PERPETUAL", od::PositionKind::Perpetual, usd, entry};
}

}  // namespace

TEST(Risk, SingleCallMatchesBlack76WhenIndexEqualsForward) {
    const auto chain = make_chain();
    const auto mkt = od::MarketView::build(chain, {});
    const auto pf = book({opt("C100", 2.0)});
    const auto r = od::compute_risk(pf, mkt);
    const auto g = od::black76_greeks(od::OptionType::Call, kF, 100000, kT, 0.5);

    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0].iv_source, od::IvSource::ExchangeMark);
    EXPECT_NEAR(r.rows[0].delta, 2 * g.delta, 1e-12);
    EXPECT_NEAR(r.equity_usd, 2 * g.price, 1e-6);
    // With index == forward, USD equity = qty * V_usd, so its delta is the Black-76 delta.
    EXPECT_NEAR(r.delta_usd_view, 2 * g.delta, 1e-5);
    // Coin view sums the premium-adjusted deltas.
    EXPECT_NEAR(r.delta_coin_view, r.rows[0].delta_premium_adj, 1e-5);
}

TEST(Risk, CoinBalanceIsLongDeltaInUsdOnly) {
    const auto mkt = od::MarketView::build(make_chain(), {});
    const auto pf = book({}, 3.0);
    const auto r = od::compute_risk(pf, mkt);
    EXPECT_NEAR(r.delta_usd_view, 3.0, 1e-9);
    EXPECT_NEAR(r.delta_coin_view, 0.0, 1e-12);

    const auto g = od::scenario_grid(pf, mkt, {0.10}, {0});
    EXPECT_NEAR(g.pnl_usd[0][0], 3.0 * kF * 0.10, 1e-6);
    EXPECT_NEAR(g.pnl_coin[0][0], 0.0, 1e-12);
}

// The classic coin-margined hedge: hold B coins, short B * price of inverse perp.
// USD equity becomes exactly constant, whatever the price does.
TEST(Risk, ShortInversePerpMakesCoinBalanceUsdNeutral) {
    const auto mkt = od::MarketView::build(make_chain(), {});
    const double B = 2.0;
    const auto pf = book({perp(-B * kF, kF)}, B);
    const auto r = od::compute_risk(pf, mkt);
    EXPECT_NEAR(r.delta_usd_view, 0.0, 1e-9);

    const auto g = od::scenario_grid(pf, mkt, {-0.5, -0.1, 0.1, 1.0}, {0});
    for (double x : g.pnl_usd[0]) EXPECT_NEAR(x, 0.0, 1e-6);
    // ...while coin equity moves: you hold fewer coins after the price rises.
    EXPECT_NEAR(g.pnl_coin[0][2], B / 1.1 - B, 1e-12);
}

TEST(Risk, PerpPnlFormula) {
    const auto mkt = od::MarketView::build(make_chain(), {});
    const auto pf = book({perp(50000, 95000)});
    const auto r = od::compute_risk(pf, mkt);
    EXPECT_NEAR(r.rows[0].pnl_coin, 50000 * (1 / 95000.0 - 1 / kF), 1e-15);
}

TEST(Risk, ScenarioGridConsistentWithGreeks) {
    const auto mkt = od::MarketView::build(make_chain(), {});
    const auto pf = book({opt("C100", -5), opt("P90", 3), opt("C120", 4)}, 1.0);
    const auto r = od::compute_risk(pf, mkt);
    const auto g = od::scenario_grid(pf, mkt, {-0.01, 0.0, 0.01}, {0, 1, -1});

    EXPECT_NEAR(g.pnl_usd[0][1], 0.0, 1e-9);  // no shock, no P&L
    // Second-order Taylor in spot: dE ~ delta*dS + 0.5*gamma_1pct*dS*(1%), in USD.
    const double dS = 0.01 * kF;
    for (int sign : {-1, 1}) {
        const double approx = sign * r.delta_usd_view * dS + 0.5 * r.gamma_usd_view * dS;
        EXPECT_NEAR(g.pnl_usd[0][sign < 0 ? 0 : 2], approx, 0.02 * std::abs(approx) + 1.0);
    }
    // Vega (USD per vol point, index == forward) vs a central difference over +-1 pt.
    // A one-sided bump would also pick up volga: this book is short ATM / long OTM vega.
    EXPECT_NEAR(0.5 * (g.pnl_usd[1][1] - g.pnl_usd[2][1]), r.vega_usd, 0.002 * std::abs(r.vega_usd));
    // One day forward ~ theta.
    const auto d1 = od::scenario_grid(pf, mkt, {0.0}, {0}, 1.0);
    EXPECT_NEAR(d1.pnl_usd[0][0], r.theta_usd, 0.01 * std::abs(r.theta_usd));
}

TEST(Risk, VegaBucketsSumToTotal) {
    const auto mkt = od::MarketView::build(make_chain(), {});
    const auto r = od::compute_risk(book({opt("C100", 1), opt("P90", -2)}), mkt);
    double sum = 0;
    for (const auto& [_, v] : r.vega_by_expiry) sum += v;
    EXPECT_NEAR(sum, r.vega_usd, 1e-9);
}

TEST(Risk, StickyStrikeVsStickyMoneyness) {
    auto chain = make_chain();
    od::SmileFit fit;
    fit.slice = &chain.expiries[0];
    fit.svi.params = {0.04, 0.2, -0.6, 0.0, 0.2};  // strong put skew
    fit.svi.ok = true;
    const auto mkt = od::MarketView::build(chain, {fit});
    const auto pf = book({opt("P90", 1)});

    const auto r = od::compute_risk(pf, mkt);
    EXPECT_EQ(r.rows[0].iv_source, od::IvSource::Svi);
    EXPECT_NEAR(r.rows[0].iv, fit.svi.params.iv(std::log(0.9), kT), 1e-12);

    // Spot up 10%: under sticky-moneyness the 90k put is now further OTM, so it is
    // priced with the (higher) skewed wing vol; under sticky-strike its vol is unchanged.
    const auto mny = od::scenario_grid(pf, mkt, {0.10}, {0}, 0, false);
    const auto stk = od::scenario_grid(pf, mkt, {0.10}, {0}, 0, true);
    EXPECT_GT(mny.pnl_usd[0][0], stk.pnl_usd[0][0]);
}

TEST(Risk, UnknownInstrumentIsReportedNotPriced) {
    const auto mkt = od::MarketView::build(make_chain(), {});
    const auto r = od::compute_risk(book({opt("NOPE", 1), opt("C100", 1)}), mkt);
    EXPECT_EQ(r.unpriced, 1);
    EXPECT_FALSE(r.rows[0].priced);
    EXPECT_TRUE(r.rows[1].priced);
}
