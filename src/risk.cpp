#include "od/risk.hpp"

#include <algorithm>
#include <cmath>

namespace od {

const char* to_string(IvSource s) {
    switch (s) {
        case IvSource::Svi: return "svi";
        case IvSource::ExchangeMark: return "mark";
        case IvSource::None: return "-";
    }
    return "?";
}

MarketView MarketView::build(const OptionChain& chain, const std::vector<SmileFit>& fits) {
    MarketView v;
    v.asof_ms_ = chain.asof_ms;
    for (const auto& slice : chain.expiries) {
        ExpiryMarket em{slice.expiry_ms, slice.forward, slice.T, std::nullopt};
        for (const auto& f : fits)
            if (f.slice->expiry_ms == slice.expiry_ms) em.smile = f.svi.params;
        v.expiries_.emplace(slice.expiry_ms, em);
        for (const auto& q : slice.quotes) v.quotes_.emplace(q.name, q);
    }
    v.index_ = chain.index_price;
    if (v.index_ <= 0.0 && !chain.expiries.empty()) v.index_ = chain.expiries.front().forward;
    return v;
}

const OptionQuote* MarketView::quote(const std::string& name) const {
    const auto it = quotes_.find(name);
    return it == quotes_.end() ? nullptr : &it->second;
}

const ExpiryMarket* MarketView::expiry(std::int64_t expiry_ms) const {
    const auto it = expiries_.find(expiry_ms);
    return it == expiries_.end() ? nullptr : &it->second;
}

namespace {

constexpr double kMinVol = 0.001;

struct OptionInputs {
    double F, T, iv;
    IvSource src;
};

// IV for an option under a shock. Sticky-moneyness reads the smile at the shocked
// forward (a strike that was 10% OTM stays priced like a 10% OTM option); sticky-strike
// keeps each strike's vol where it was.
std::optional<OptionInputs> option_inputs(const OptionQuote& q, const ExpiryMarket& em,
                                          const Shock& sh) {
    const double F = em.forward * (1.0 + sh.spot);
    const double T = em.T - sh.days / 365.0;
    double iv = 0.0;
    IvSource src = IvSource::None;
    if (em.smile) {
        const double k = std::log(q.strike / (sh.sticky_strike ? em.forward : F));
        iv = em.smile->iv(k, em.T);
        src = IvSource::Svi;
    }
    if (iv <= 0.0 && q.mark_iv > 0.0) {
        iv = q.mark_iv;
        src = IvSource::ExchangeMark;
    }
    if (src == IvSource::None) return std::nullopt;
    return OptionInputs{F, T, std::max(iv + sh.vol_pts / 100.0, kMinVol), src};
}

double perp_coin_pnl(const Position& p, double price) {
    return p.qty * (1.0 / p.entry_price - 1.0 / price);
}

}  // namespace

Valuation value_portfolio(const Portfolio& pf, const MarketView& mkt, const Shock& sh) {
    const double index = mkt.index() * (1.0 + sh.spot);
    double coin = pf.coin_balance;
    for (const auto& p : pf.positions) {
        if (p.kind == PositionKind::Perpetual) {
            coin += perp_coin_pnl(p, index);
            continue;
        }
        const auto* q = mkt.quote(p.instrument);
        const auto* em = q ? mkt.expiry(q->expiry_ms) : nullptr;
        if (!em) continue;
        const auto in = option_inputs(*q, *em, sh);
        if (!in) continue;
        const double usd = black76_price(q->type, in->F, q->strike, in->T, in->iv);
        coin += p.qty * usd_to_coin(usd, in->F);
    }
    return {coin, coin * index};
}

RiskReport compute_risk(const Portfolio& pf, const MarketView& mkt) {
    RiskReport r;
    const double index = mkt.index();

    for (const auto& p : pf.positions) {
        PositionRisk row;
        row.position = &p;
        if (p.kind == PositionKind::Perpetual) {
            row.priced = true;
            row.mark_coin = index;  // USD price for the perp
            row.pnl_coin = perp_coin_pnl(p, index);
            // USD value = index * qty * (1/entry - 1/index) = qty * (index/entry - 1)
            row.delta = p.qty / p.entry_price;
            row.delta_premium_adj = p.qty / index;  // index * d(coin pnl)/d(index)
            r.rows.push_back(row);
            continue;
        }

        const auto* q = mkt.quote(p.instrument);
        const auto* em = q ? mkt.expiry(q->expiry_ms) : nullptr;
        const auto in = em ? option_inputs(*q, *em, {}) : std::nullopt;
        if (!in) {
            ++r.unpriced;
            r.rows.push_back(row);
            continue;
        }
        const auto g = black76_greeks(q->type, in->F, q->strike, in->T, in->iv);
        row.priced = true;
        row.expiry_ms = q->expiry_ms;
        row.forward = in->F;
        row.T = in->T;
        row.iv = in->iv;
        row.iv_source = in->src;
        row.mark_coin = usd_to_coin(g.price, in->F);
        row.pnl_coin = p.qty * (row.mark_coin - p.entry_price);
        row.delta = p.qty * g.delta;
        row.delta_premium_adj = p.qty * inverse_delta(g.delta, g.price, in->F);
        row.gamma_1pct = p.qty * g.gamma * 0.01 * in->F;
        row.vega_usd = p.qty * g.vega;
        row.theta_usd = p.qty * g.theta;

        r.vega_usd += row.vega_usd;
        r.theta_usd += row.theta_usd;
        r.vega_by_expiry[q->expiry_ms] += row.vega_usd;
        r.rows.push_back(row);
    }

    const auto base = value_portfolio(pf, mkt);
    r.equity_coin = base.equity_coin;
    r.equity_usd = base.equity_usd;

    // Central differences on the full valuation. h = 0.1% is small enough for the
    // curvature not to bias delta and large enough to stay clear of rounding noise.
    constexpr double h = 0.001;
    auto delta_usd_at = [&](double s) {
        const double up = value_portfolio(pf, mkt, {.spot = s + h}).equity_usd;
        const double dn = value_portfolio(pf, mkt, {.spot = s - h}).equity_usd;
        return (up - dn) / (2.0 * h * index * (1.0 + s));
    };
    r.delta_usd_view = delta_usd_at(0.0);
    r.gamma_usd_view = 0.5 * (delta_usd_at(0.01) - delta_usd_at(-0.01));
    const double cu = value_portfolio(pf, mkt, {.spot = h}).equity_coin;
    const double cd = value_portfolio(pf, mkt, {.spot = -h}).equity_coin;
    r.delta_coin_view = (cu - cd) / (2.0 * h);
    return r;
}

ScenarioGrid scenario_grid(const Portfolio& pf, const MarketView& mkt, std::vector<double> spot,
                           std::vector<double> vol_pts, double days, bool sticky_strike) {
    ScenarioGrid g{std::move(spot), std::move(vol_pts), days, {}, {}};
    const auto base = value_portfolio(pf, mkt);
    for (double v : g.vol_pts) {
        auto& row_usd = g.pnl_usd.emplace_back();
        auto& row_coin = g.pnl_coin.emplace_back();
        for (double s : g.spot) {
            const auto val = value_portfolio(pf, mkt, {s, v, days, sticky_strike});
            row_usd.push_back(val.equity_usd - base.equity_usd);
            row_coin.push_back(val.equity_coin - base.equity_coin);
        }
    }
    return g;
}

}  // namespace od
