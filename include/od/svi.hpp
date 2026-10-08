#pragma once
// SVI ("stochastic volatility inspired", Gatheral 2004) smile for one expiry.
//
// Raw parameterisation, in total implied variance w = sigma_iv^2 * T against
// log-moneyness k = ln(K / F):
//
//     w(k) = a + b * ( rho * (k - m) + sqrt((k - m)^2 + s^2) )
//
//   a   : overall variance level        b   : wing steepness
//   rho : skew (put wing vs call wing)  m   : horizontal shift
//   s   : ATM curvature (smoothness of the vertex)
//
// Calibration uses the quasi-explicit method (Zeliade 2009): for fixed (m, s)
// the model is linear in (a, b*rho*s, b*s), so those come from a 3x3 weighted
// least-squares solve and only (m, s) need a numerical search.

#include <span>
#include <vector>

namespace od {

struct SviParams {
    double a = 0.0, b = 0.0, rho = 0.0, m = 0.0, s = 0.1;

    double w(double k) const;    // total variance
    double w1(double k) const;   // dw/dk
    double w2(double k) const;   // d2w/dk2
    double iv(double k, double T) const;

    // Durrleman's condition: the implied risk-neutral density is non-negative
    // at k iff g(k) >= 0. g < 0 anywhere means a butterfly spread with negative
    // cost exists, i.e. static arbitrage.
    double g(double k) const;

    // Gatheral's parameter constraints plus Roger Lee's wing bound b(1+|rho|) <= 2.
    bool is_valid() const;
};

struct SviQuote {
    double k;       // log-moneyness ln(K/F)
    double w;       // market total variance iv^2 * T
    double weight;  // multiplies the squared residual in w
};

struct SviFit {
    SviParams params;
    double rmse_w = 0.0;  // weighted RMS residual in total variance
    int n = 0;
    bool ok = false;
};

SviFit fit_svi(std::span<const SviQuote> quotes);

// Minimum of g(k) over a grid on [k_lo, k_hi]. Negative => butterfly arbitrage.
double min_durrleman_g(const SviParams& p, double k_lo = -1.5, double k_hi = 1.5, int n = 301);

}  // namespace od
