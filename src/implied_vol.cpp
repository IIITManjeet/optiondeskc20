#include "od/implied_vol.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace od {

const char* to_string(IvStatus s) {
    switch (s) {
        case IvStatus::Ok: return "ok";
        case IvStatus::BelowIntrinsic: return "below_intrinsic";
        case IvStatus::AboveMax: return "above_max";
        case IvStatus::NoConvergence: return "no_convergence";
        case IvStatus::BadInput: return "bad_input";
    }
    return "?";
}

IvResult implied_vol(OptionType type, double price_usd, double F, double K, double T, double r) {
    IvResult res;
    if (!(price_usd >= 0.0) || !(F > 0.0) || !(K > 0.0) || !(T > 0.0)) return res;

    // Work with the undiscounted price so the bounds are simple.
    const double target = price_usd * std::exp(r * T);
    const bool call = type == OptionType::Call;
    const double intrinsic = std::max(call ? F - K : K - F, 0.0);
    const double upper = call ? F : K;

    const double price_tol = 1e-12 * F;
    if (target < intrinsic - price_tol) {
        res.status = IvStatus::BelowIntrinsic;
        return res;
    }
    if (target >= upper) {
        res.status = IvStatus::AboveMax;
        return res;
    }
    if (target - intrinsic <= price_tol) {
        res.status = IvStatus::BelowIntrinsic;  // no time value left to invert
        return res;
    }

    auto price_at = [&](double s) { return black76_price(type, F, K, T, s); };

    double lo = 1e-6, hi = 5.0;
    double p_hi = price_at(hi);
    while (p_hi < target && hi < 100.0) p_hi = price_at(hi *= 2.0);
    if (p_hi < target) {
        res.status = IvStatus::AboveMax;
        return res;
    }

    // Price and vega share d1, so compute them together.
    const double log_fk = std::log(F / K), sqrt_t = std::sqrt(T);
    auto price_vega = [&](double sig, double& vega) {
        const double sd = sig * sqrt_t;
        const double d1 = log_fk / sd + 0.5 * sd, d2 = d1 - sd;
        vega = F * norm_pdf(d1) * sqrt_t;
        return call ? F * norm_cdf(d1) - K * norm_cdf(d2) : K * norm_cdf(-d2) - F * norm_cdf(-d1);
    };

    // Brenner-Subrahmanyam: ATM price ~ 0.4 * F * sigma * sqrt(T). Decent seed near the money.
    double s = std::sqrt(2.0 * std::numbers::pi / T) * (target - intrinsic) / F;
    s = std::clamp(s, 0.05, 3.0);

    for (int i = 1; i <= 100; ++i) {
        double vega;
        const double f = price_vega(s, vega) - target;
        res.iterations = i;
        if (std::abs(f) <= price_tol) {
            res.sigma = s;
            res.status = IvStatus::Ok;
            return res;
        }
        (f > 0.0 ? hi : lo) = s;

        double next = vega > 0.0 ? s - f / vega : -1.0;
        if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);

        if (std::abs(next - s) < 1e-14) {
            res.sigma = next;
            res.status = IvStatus::Ok;
            return res;
        }
        s = next;
    }
    res.sigma = s;
    res.status = IvStatus::NoConvergence;
    return res;
}

}  // namespace od
