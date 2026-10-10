#pragma once
// Approximate Deribit fees (check the current schedule before relying on them):
// options 0.03% of the underlying per contract, i.e. 0.0003 coin, capped at 12.5%
// of the option price; perpetual taker 0.05% of notional.

#include <algorithm>
#include <cmath>

namespace od {

inline double option_fee_coin(double price_coin, double qty) {
    return std::min(0.0003, 0.125 * price_coin) * qty;
}

inline double perp_taker_fee_coin(double usd, double price) { return 0.0005 * std::abs(usd) / price; }

}  // namespace od
