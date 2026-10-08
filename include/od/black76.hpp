#pragma once
// Black-76: European options on a forward/future.
//
// Deribit options settle against an index but are marked off the forward for
// their expiry (`underlying_price` in the API). Pricing on the forward means the
// carry (funding, basis) is already inside F, so we don't need a separate
// dividend/borrow model like Black-Scholes on spot would.
//
// Conventions (chosen to match what Deribit reports, verified against live data):
//   price  : USD per 1 unit of underlying
//   delta  : dV/dF
//   gamma  : d2V/dF2
//   vega   : dV/dsigma per 1 vol point (0.01)
//   theta  : dV/dt per calendar day (365-day year), i.e. -dV/dT / 365

#include <cmath>
#include <numbers>

namespace od {

enum class OptionType { Call, Put };

inline double norm_pdf(double x) {
    return std::exp(-0.5 * x * x) * (1.0 / std::sqrt(2.0 * std::numbers::pi));
}

inline double norm_cdf(double x) {
    // erfc keeps precision in the far left tail where 1 + erf(x) cancels.
    return 0.5 * std::erfc(-x * std::numbers::sqrt2 / 2.0);
}

struct Greeks {
    double price;
    double delta;
    double gamma;
    double vega;
    double theta;
};

namespace detail {
struct D12 {
    double d1, d2, sqrt_t;
};
inline D12 d12(double F, double K, double T, double sigma) {
    const double sqrt_t = std::sqrt(T);
    const double sd = sigma * sqrt_t;
    const double d1 = (std::log(F / K) + 0.5 * sd * sd) / sd;
    return {d1, d1 - sd, sqrt_t};
}
}  // namespace detail

inline double black76_price(OptionType type, double F, double K, double T, double sigma,
                            double r = 0.0) {
    const double df = std::exp(-r * T);
    if (T <= 0.0 || sigma <= 0.0) {
        const double intrinsic = type == OptionType::Call ? F - K : K - F;
        return df * (intrinsic > 0.0 ? intrinsic : 0.0);
    }
    const auto [d1, d2, sqrt_t] = detail::d12(F, K, T, sigma);
    return type == OptionType::Call ? df * (F * norm_cdf(d1) - K * norm_cdf(d2))
                                    : df * (K * norm_cdf(-d2) - F * norm_cdf(-d1));
}

// dV/dsigma per 1.0 of vol (not per vol point). Used by the IV solver.
inline double black76_vega_raw(double F, double K, double T, double sigma, double r = 0.0) {
    if (T <= 0.0 || sigma <= 0.0) return 0.0;
    const auto [d1, d2, sqrt_t] = detail::d12(F, K, T, sigma);
    return std::exp(-r * T) * F * norm_pdf(d1) * sqrt_t;
}

inline Greeks black76_greeks(OptionType type, double F, double K, double T, double sigma,
                             double r = 0.0) {
    if (T <= 0.0 || sigma <= 0.0) {
        const double price = black76_price(type, F, K, T, sigma, r);
        const bool itm = type == OptionType::Call ? F > K : K > F;
        const double delta = itm ? (type == OptionType::Call ? 1.0 : -1.0) : 0.0;
        return {price, delta, 0.0, 0.0, 0.0};
    }
    const double df = std::exp(-r * T);
    const auto [d1, d2, sqrt_t] = detail::d12(F, K, T, sigma);
    const double pdf = norm_pdf(d1);
    const bool call = type == OptionType::Call;

    const double price = call ? df * (F * norm_cdf(d1) - K * norm_cdf(d2))
                              : df * (K * norm_cdf(-d2) - F * norm_cdf(-d1));
    const double delta = call ? df * norm_cdf(d1) : -df * norm_cdf(-d1);
    const double gamma = df * pdf / (F * sigma * sqrt_t);
    const double vega = df * F * pdf * sqrt_t;
    // V = df * undiscounted(T)  =>  dV/dT = -r V + df * F * pdf * sigma / (2 sqrt T)
    const double dv_dT = -r * price + df * F * pdf * sigma / (2.0 * sqrt_t);

    return {price, delta, gamma, vega / 100.0, -dv_dT / 365.0};
}

// --- Inverse (coin-settled) contracts --------------------------------------
// Deribit BTC/ETH options are quoted and settled in the coin: price_coin = V_usd / F.

inline double usd_to_coin(double price_usd, double F) { return price_usd / F; }
inline double coin_to_usd(double price_coin, double F) { return price_coin * F; }

// Premium-adjusted delta: the hedge ratio when your P&L is measured in the coin,
// because the premium itself moves with the coin price.
inline double inverse_delta(double delta_usd, double price_usd, double F) {
    return delta_usd - price_usd / F;
}

}  // namespace od
