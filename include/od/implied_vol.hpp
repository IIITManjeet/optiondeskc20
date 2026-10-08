#pragma once
// Implied volatility: invert Black-76 for sigma given a market price.
//
// Safeguarded Newton: Newton steps on vega while they stay inside a bracket
// [lo, hi] that always contains the root, falling back to bisection otherwise.
// Newton alone diverges for deep OTM / short-dated options where vega ~ 0;
// bisection alone is slow. The bracket keeps it robust, Newton keeps it fast.
//
// Upgrade path: Jaeckel's "Let's Be Rational" reaches machine precision in two
// iterations, but it is far harder to read; this is the version to learn from.

#include "od/black76.hpp"

namespace od {

enum class IvStatus {
    Ok,
    BelowIntrinsic,  // price < intrinsic value: no sigma reproduces it (arbitrage / stale quote)
    AboveMax,        // price >= upper bound (F for calls, K for puts)
    NoConvergence,
    BadInput,
};

const char* to_string(IvStatus s);

struct IvResult {
    double sigma = 0.0;
    int iterations = 0;
    IvStatus status = IvStatus::BadInput;
    bool ok() const { return status == IvStatus::Ok; }
};

IvResult implied_vol(OptionType type, double price_usd, double F, double K, double T,
                     double r = 0.0);

}  // namespace od
