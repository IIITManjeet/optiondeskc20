#include "od/quoter.hpp"

#include <algorithm>
#include <cmath>

#include "od/implied_vol.hpp"

namespace od {

double option_tick(double price_coin) { return price_coin >= 0.005 ? 0.0005 : 0.0001; }

double round_to_tick(double px, bool down) {
    // Tick depends on the price itself; near the 0.005 boundary use the finer tick
    // below it and the coarser one above.
    const double tick = option_tick(px);
    const double n = px / tick;
    // Small epsilon so values already on the grid don't move by floating-point noise.
    const double r = down ? std::floor(n + 1e-9) : std::ceil(n - 1e-9);
    return r * tick;
}

Quote make_quote(const QuoteInputs& in, const QuoteParams& p) {
    Quote q;
    q.size = p.size;
    if (!(in.forward > 0.0) || !(in.T > 0.0) || !(in.theo_iv > 0.0)) return q;

    auto coin_price = [&](double vol) {
        return usd_to_coin(black76_price(in.type, in.forward, in.strike, in.T, std::max(vol, 0.01)),
                           in.forward);
    };
    q.theo_coin = coin_price(in.theo_iv);

    q.skew_vol = std::clamp(-p.skew_vol_per_1k_vega * in.portfolio_vega_usd / 1000.0, -p.max_skew_vol,
                            p.max_skew_vol);
    q.bid_iv = in.theo_iv + q.skew_vol - p.half_spread_vol;
    q.ask_iv = in.theo_iv + q.skew_vol + p.half_spread_vol;

    double bid = round_to_tick(coin_price(q.bid_iv), /*down=*/true);
    double ask = round_to_tick(coin_price(q.ask_iv), /*down=*/false);

    // Post-only: never cross the book. Back off to one tick inside the other side.
    if (in.best_ask > 0.0 && bid >= in.best_ask)
        bid = round_to_tick(in.best_ask - option_tick(in.best_ask - 1e-12), true);
    if (in.best_bid > 0.0 && ask <= in.best_bid)
        ask = round_to_tick(in.best_bid + option_tick(in.best_bid), false);

    // How far did rounding (and post-only clamping) move each side, in vol?
    auto vol_of = [&](double coin) {
        const auto r = implied_vol(in.type, coin_to_usd(coin, in.forward), in.forward, in.strike, in.T);
        return r.ok() ? r.sigma : -1.0;
    };
    const bool bid_ok = bid >= option_tick(0.0) && std::abs(vol_of(bid) - q.bid_iv) <= p.max_tick_vol;
    const bool ask_ok = ask > 0.0 && std::abs(vol_of(ask) - q.ask_iv) <= p.max_tick_vol;

    const bool can_buy = in.position + p.size <= p.max_position + 1e-12;
    const bool can_sell = in.position - p.size >= -p.max_position - 1e-12;
    if (can_buy && bid_ok) q.bid = bid;
    if (can_sell && ask_ok) q.ask = ask;
    if (q.bid && q.ask && *q.bid >= *q.ask) q.bid.reset();  // degenerate after clamping
    return q;
}

}  // namespace od
