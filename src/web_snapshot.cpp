#include "od/web_snapshot.hpp"

#include <algorithm>
#include <cmath>

#include "od/black76.hpp"

namespace od::web {

namespace {

using nlohmann::json;

double finite(double x) { return std::isfinite(x) ? x : 0.0; }

json quote_json(const OptionQuote& q, const ExpirySlice& s) {
    const double iv = q.mark_iv > 0 ? q.mark_iv : 0.0;
    const auto g = iv > 0 ? black76_greeks(q.type, s.forward, q.strike, s.T, iv) : Greeks{0, 0, 0, 0, 0};
    return {{"name", q.name},   {"bid", q.bid},          {"ask", q.ask},
            {"mark", q.mark},   {"mark_iv", q.mark_iv},  {"oi", q.open_interest},
            {"delta", g.delta}, {"gamma", g.gamma * s.forward / 100},  // per 1% move
            {"vega", g.vega},   {"theta", g.theta}};                    // USD per vol pt / day
}

}  // namespace

json snapshot_json(const SnapshotInput& in, const std::vector<SmileFit>& fits, double fit_ms,
                   std::int64_t server_ms) {
    json out{{"type", "snapshot"}, {"server_ms", server_ms}};
    if (!in.chain) {
        out["feed"] = nullptr;
        out["waiting_for"] = in.feed.bus;
        return out;
    }
    const auto& chain = *in.chain;
    const auto& f = in.feed;
    const auto& c = f.stats;
    out["currency"] = chain.currency;
    out["asof_ms"] = chain.asof_ms;
    out["index"] = chain.index_price;
    out["fit_ms"] = fit_ms;
    out["feed"] = {{"bus", f.bus},
                   {"connected", c.connected},
                   {"stale", f.stale},
                   {"subscribed", c.subscribed},
                   {"instruments", f.n_instruments},
                   {"live", f.live},
                   {"updates_per_s", f.updates_per_s},
                   {"trades_per_s", f.trades_per_s},
                   {"hop_p50_us", f.hop_p50_us},
                   {"hop_p99_us", f.hop_p99_us},
                   {"frames", c.frames},
                   {"tickers", c.tickers},
                   {"trades", c.trades},
                   {"heartbeats", c.heartbeats},
                   {"reconnects", c.reconnects},
                   {"drops", c.ring_full},
                   {"laps", f.laps}};

    json expiries = json::array();
    for (const auto& s : chain.expiries) {
        json e{{"label", s.label}, {"expiry_ms", s.expiry_ms}, {"days", s.T * 365.0},
               {"forward", s.forward}};
        const auto fit = std::find_if(fits.begin(), fits.end(), [&](const auto& x) { return x.slice == &s; });
        if (fit != fits.end() && fit->svi.ok) {
            const auto& p = fit->svi.params;
            e["fit"] = {{"atm_iv", finite(fit->atm_iv)},  {"rr25", finite(fit->rr25)},
                        {"bf25", finite(fit->bf25)},      {"rmse", fit->rmse_vol},
                        {"n_fit", fit->n_fit},            {"n_inside", fit->n_inside},
                        {"min_g", fit->min_g},            {"calendar", fit->calendar_violation},
                        {"k_lo", fit->k_lo},              {"k_hi", fit->k_hi},
                        {"svi", {{"a", p.a}, {"b", p.b}, {"rho", p.rho}, {"m", p.m}, {"s", p.s}}}};
            // The fitted smile, a little past the quoted range on each side.
            const double pad = 0.15 * (fit->k_hi - fit->k_lo) + 0.01;
            json curve = json::array();
            for (int i = 0; i <= 80; ++i) {
                const double k = fit->k_lo - pad + (fit->k_hi - fit->k_lo + 2 * pad) * i / 80.0;
                curve.push_back({k, finite(p.iv(k, s.T))});
            }
            e["curve"] = std::move(curve);
            json pts = json::array();
            for (const auto& x : fit->strikes)
                pts.push_back({{"strike", x.quote->strike},
                               {"k", x.k},
                               {"type", x.quote->type == OptionType::Call ? "C" : "P"},
                               {"bid_iv", x.bid_iv},
                               {"ask_iv", x.ask_iv},
                               {"mid_iv", x.mid_iv},
                               {"mark_iv", x.quote->mark_iv},
                               {"in_fit", x.in_fit}});
            e["points"] = std::move(pts);
        }
        // Chain rows: one per strike, call and put side by side.
        json rows = json::array();
        for (std::size_t i = 0; i < s.quotes.size();) {
            const double K = s.quotes[i].strike;
            json row{{"strike", K}, {"call", nullptr}, {"put", nullptr}};
            for (; i < s.quotes.size() && s.quotes[i].strike == K; ++i)
                row[s.quotes[i].type == OptionType::Call ? "call" : "put"] = quote_json(s.quotes[i], s);
            rows.push_back(std::move(row));
        }
        e["chain"] = std::move(rows);
        expiries.push_back(std::move(e));
    }
    out["expiries"] = std::move(expiries);

    json trades = json::array();
    for (const auto& t : in.trades)
        trades.push_back({{"name", t.name}, {"ts_ms", t.ts_ms}, {"side", t.buy ? "buy" : "sell"},
                          {"price", t.price}, {"amount", t.amount}, {"iv", t.iv}, {"index", t.index}});
    out["trades"] = std::move(trades);
    return out;
}

}  // namespace od::web
