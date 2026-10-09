#pragma once
// Option quoting in volatility space.
//
// An options market maker doesn't think "bid 0.0215, offer 0.0225"; it thinks
// "I'll buy at 36.5 vol and sell at 38.5" and converts to prices. Vol is
// comparable across strikes and expiries; price isn't.
//
//   theo vol   : fitted SVI smile at the option's current moneyness
//   skew       : shift both sides against inventory. Long vega -> lower both
//                quotes: sell more eagerly, buy less eagerly (Avellaneda-Stoikov
//                inventory term, expressed in vol and driven by portfolio vega)
//   half spread: what we charge for liquidity, in vol points
//
// Then: convert to coin prices, round outward to Deribit's tick ladder, stay
// post-only (never cross the current book), and drop a side when the position
// limit for that direction is reached.

#include <optional>

#include "od/black76.hpp"

namespace od {

// Deribit option tick ladder: 0.0001 below 0.005 coin, 0.0005 at and above.
double option_tick(double price_coin);
double round_to_tick(double price_coin, bool down);

struct QuoteParams {
    double half_spread_vol = 0.015;     // 1.5 vol points each side
    double skew_vol_per_1k_vega = 0.005;  // 0.5 vol pts of skew per $1,000 of portfolio vega
    double max_skew_vol = 0.05;
    double size = 0.5;                  // contracts per side
    double max_position = 3.0;          // per instrument, contracts
};

struct QuoteInputs {
    OptionType type = OptionType::Call;
    double strike = 0.0;
    double forward = 0.0;
    double T = 0.0;
    double theo_iv = 0.0;
    double best_bid = 0.0, best_ask = 0.0;  // coin, 0 = no quote on that side
    double position = 0.0;                  // contracts in this instrument
    double portfolio_vega_usd = 0.0;        // USD per vol point
};

struct Quote {
    std::optional<double> bid, ask;  // coin prices
    double size = 0.0;
    double theo_coin = 0.0;
    double bid_iv = 0.0, ask_iv = 0.0;
    double skew_vol = 0.0;
};

Quote make_quote(const QuoteInputs& in, const QuoteParams& p);

}  // namespace od
