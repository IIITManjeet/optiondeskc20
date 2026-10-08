#pragma once
// Volatility surface built from a chain: market IVs per strike, an SVI fit per
// expiry, standard smile metrics, and static-arbitrage diagnostics.

#include <vector>

#include "od/chain.hpp"
#include "od/svi.hpp"

namespace od {

struct StrikeIv {
    const OptionQuote* quote = nullptr;
    double k = 0.0;  // ln(K/F)
    double bid_iv = 0.0, ask_iv = 0.0, mid_iv = 0.0;  // 0 if not solvable
    double svi_iv = 0.0;
    bool in_fit = false;
};

struct SmileFit {
    const ExpirySlice* slice = nullptr;
    std::vector<StrikeIv> strikes;
    SviFit svi;
    double rmse_vol = 0.0;  // fitted vs market mid IV on fit points, decimal
    int n_fit = 0;          // quotes used in the fit
    int n_inside = 0;       // of those, how many have the SVI vol inside the bid/ask IVs
    double atm_iv = 0.0;    // SVI at k = 0 (forward ATM)
    double rr25 = 0.0;      // 25-delta risk reversal: iv(25d call) - iv(25d put)
    double bf25 = 0.0;      // 25-delta butterfly: avg(25d call, 25d put) - ATM
    double min_g = 0.0;     // Durrleman over the quoted k range; < 0 means butterfly arbitrage
    double k_lo = 0.0, k_hi = 0.0;  // log-moneyness range of the quotes used in the fit
    // Largest drop in total variance vs the previous fitted expiry, over the k range
    // both expiries were fitted on. > 0 means calendar arbitrage: a later expiry
    // implies less total variance, so selling it and buying the earlier one is free money.
    double calendar_violation = 0.0;
};

struct SurfaceOptions {
    double min_T_years = 2.0 / (24 * 365);  // skip expiries closer than 2 hours
    double max_spread_vol = 0.15;           // drop quotes with bid/ask IV spread wider than this
    int min_points = 6;
};

std::vector<SmileFit> build_surface(const OptionChain& chain, const SurfaceOptions& opt = {});

// Cross-check our pricer against the exchange: re-solve IV from each OTM
// option's mark price and compare with the exchange's published mark_iv.
//
// Deribit publishes mark_iv to 0.01 vol pts, so a correct pricer lands within
// ~0.005 pts. The book summary is not an atomic snapshot (the forward moves while
// it is assembled), which shows up as a tail on short-dated near-ATM options;
// look at the median, not just the max. Options whose vega is too small for the
// 1e-8 coin price precision to pin down IV are counted but excluded.
struct PricerCheck {
    int n = 0;
    int n_low_vega = 0;
    int iv_failures = 0;
    double median_iv_err = 0.0;  // decimal vol
    double p95_iv_err = 0.0;
    double max_iv_err = 0.0;
};

PricerCheck check_against_exchange(const OptionChain& chain);

}  // namespace od
