#include "od/surface.hpp"

#include <algorithm>
#include <cmath>

#include "od/implied_vol.hpp"

namespace od {

namespace {

double solve_iv_coin(const OptionQuote& q, double price_coin, double F, double T) {
    if (price_coin <= 0.0) return 0.0;
    const auto r = implied_vol(q.type, coin_to_usd(price_coin, F), F, q.strike, T);
    return r.ok() ? r.sigma : 0.0;
}

// Log-moneyness where the Black-76 call delta N(d1) equals `target`, using the
// fitted smile's own vol at each k. N(d1) is decreasing in k, so bisect.
double k_at_call_delta(const SviParams& p, double target) {
    double lo = -3.0, hi = 3.0;
    for (int i = 0; i < 100; ++i) {
        const double k = 0.5 * (lo + hi);
        const double w = std::max(p.w(k), 1e-12);
        const double d1 = (-k + 0.5 * w) / std::sqrt(w);
        (norm_cdf(d1) > target ? lo : hi) = k;
    }
    return 0.5 * (lo + hi);
}

}  // namespace

std::vector<SmileFit> build_surface(const OptionChain& chain, const SurfaceOptions& opt) {
    std::vector<SmileFit> out;
    const SmileFit* prev = nullptr;

    for (const auto& slice : chain.expiries) {
        if (slice.T < opt.min_T_years) continue;
        const double F = slice.forward, T = slice.T;

        SmileFit fit;
        fit.slice = &slice;
        std::vector<SviQuote> pts;
        for (const auto& q : slice.quotes) {
            StrikeIv s;
            s.quote = &q;
            s.k = std::log(q.strike / F);
            s.bid_iv = solve_iv_coin(q, q.bid, F, T);
            s.ask_iv = solve_iv_coin(q, q.ask, F, T);
            if (s.bid_iv > 0.0 && s.ask_iv > 0.0) s.mid_iv = 0.5 * (s.bid_iv + s.ask_iv);

            // Fit only out-of-the-money options: they carry the smile information,
            // while ITM prices are mostly intrinsic and their IVs are noisy.
            const bool otm = q.type == OptionType::Put ? q.strike < F : q.strike >= F;
            const double spread = s.ask_iv - s.bid_iv;
            if (otm && s.mid_iv > 0.0 && spread >= 0.0 && spread <= opt.max_spread_vol) {
                s.in_fit = true;
                // Residuals are in total variance; dw ~ 2*iv*T*d(iv). Dividing by that
                // makes them vol errors, and dividing by the spread trusts tight quotes more.
                const double scale = 2.0 * s.mid_iv * T * std::max(spread, 0.005);
                pts.push_back({s.k, s.mid_iv * s.mid_iv * T, 1.0 / (scale * scale)});
            }
            fit.strikes.push_back(s);
        }
        if (static_cast<int>(pts.size()) < opt.min_points) continue;

        fit.svi = fit_svi(pts);
        if (!fit.svi.ok) continue;
        const auto& p = fit.svi.params;

        double se = 0.0;
        int n = 0;
        for (auto& s : fit.strikes) {
            s.svi_iv = p.iv(s.k, T);
            if (!s.in_fit) continue;
            se += (s.svi_iv - s.mid_iv) * (s.svi_iv - s.mid_iv), ++n;
            fit.n_inside += s.svi_iv >= s.bid_iv && s.svi_iv <= s.ask_iv;
        }
        fit.n_fit = n;
        fit.rmse_vol = std::sqrt(se / n);
        fit.atm_iv = p.iv(0.0, T);
        const double iv_c25 = p.iv(k_at_call_delta(p, 0.25), T);
        const double iv_p25 = p.iv(k_at_call_delta(p, 0.75), T);  // put delta -0.25
        fit.rr25 = iv_c25 - iv_p25;
        fit.bf25 = 0.5 * (iv_c25 + iv_p25) - fit.atm_iv;
        fit.k_lo = fit.k_hi = pts.front().k;
        for (const auto& e : pts) fit.k_lo = std::min(fit.k_lo, e.k), fit.k_hi = std::max(fit.k_hi, e.k);
        fit.min_g = min_durrleman_g(p, fit.k_lo, fit.k_hi);

        if (prev) {
            const auto& pp = prev->svi.params;
            const double lo = std::max(fit.k_lo, prev->k_lo), hi = std::min(fit.k_hi, prev->k_hi);
            for (int i = 0; i <= 200 && lo < hi; ++i) {
                const double k = lo + (hi - lo) * i / 200;
                fit.calendar_violation = std::max(fit.calendar_violation, pp.w(k) - p.w(k));
            }
        }
        out.push_back(std::move(fit));
        prev = &out.back();
    }
    return out;
}

PricerCheck check_against_exchange(const OptionChain& chain) {
    PricerCheck c;
    std::vector<double> errs;
    for (const auto& slice : chain.expiries) {
        for (const auto& q : slice.quotes) {
            if (q.mark <= 0.0 || q.mark_iv <= 0.0 || q.underlying <= 0.0) continue;
            const double F = q.underlying;
            // OTM only: Deribit's deep-ITM marks sit closer to intrinsic than their own
            // published mark_iv implies, so those aren't a test of our pricer.
            if (q.type == OptionType::Call ? q.strike < F : q.strike > F) continue;
            ++c.n;
            // Coin per 1.0 of vol. At 1e-5 per vol point, a 1e-8 price tick moves IV by 0.001 pts.
            const double vega = usd_to_coin(black76_vega_raw(F, q.strike, slice.T, q.mark_iv), F);
            if (vega < 1e-3) {
                ++c.n_low_vega;
                continue;
            }
            const auto iv = implied_vol(q.type, coin_to_usd(q.mark, F), F, q.strike, slice.T);
            if (!iv.ok()) {
                ++c.iv_failures;
                continue;
            }
            errs.push_back(std::abs(iv.sigma - q.mark_iv));
        }
    }
    if (!errs.empty()) {
        std::sort(errs.begin(), errs.end());
        c.median_iv_err = errs[errs.size() / 2];
        c.p95_iv_err = errs[errs.size() * 95 / 100];
        c.max_iv_err = errs.back();
    }
    return c;
}

}  // namespace od
