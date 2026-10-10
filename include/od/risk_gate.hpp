#pragma once
// Pre-trade risk gate: every order passes through check() before reaching the
// gateway. Limits are evaluated on the worst case (all live orders on the same
// side fill), and orders that *reduce* the risk being limited are always allowed,
// so the gate never traps the strategy in a position it can't get out of.

#include <cstdint>
#include <optional>
#include <string>

#include "od/orders.hpp"

namespace od {

struct RiskLimits {
    double max_order_qty = 1.0;        // contracts
    double max_position = 3.0;         // per instrument, contracts
    double max_abs_vega_usd = 3000.0;  // portfolio, USD per vol point
    // Per expiry, USD per vol point. Vega in different expiries doesn't net out in
    // practice (dailies move on very different things from 2-month vol), so each
    // expiry gets its own limit. 0 disables.
    double max_abs_vega_per_expiry_usd = 1500.0;
    double price_band_vol = 0.10;      // order IV must be within this of theo (fat-finger)
    double max_orders_per_sec = 20.0;  // token bucket refill rate
    double burst = 40.0;               // token bucket size
};

struct GateContext {
    double position = 0.0;            // current, this instrument
    double open_same_side = 0.0;      // remaining qty of live orders on the order's side
    double portfolio_vega_usd = 0.0;
    double expiry_vega_usd = 0.0;     // current vega in the order's expiry
    // Signed vega of our other live orders that push vega the same way as this one,
    // portfolio-wide and within this expiry: the worst case assumes they all fill.
    double open_vega_same_dir_usd = 0.0;
    double open_expiry_vega_same_dir_usd = 0.0;
    double vega_per_contract_usd = 0.0;
    double theo_iv = 0.0;
    double order_iv = 0.0;            // IV implied by the order price (0 = not computable)
};

class RiskGate {
public:
    explicit RiskGate(RiskLimits limits) : lim_(limits), tokens_(limits.burst) {}

    // nullopt = accepted (and one rate token consumed); otherwise the reject reason.
    std::optional<std::string> check(const OrderRequest& req, const GateContext& ctx,
                                     std::int64_t now_ns);

    // Cancels are never refused, but they use the same exchange rate limit as new
    // orders, so they spend a token too (the bucket can go negative, down to -burst,
    // which delays the next new orders).
    void spend(std::int64_t now_ns);

    void kill() { killed_ = true; }
    bool killed() const { return killed_; }
    const RiskLimits& limits() const { return lim_; }

private:
    RiskLimits lim_;
    double tokens_;
    std::int64_t last_ns_ = 0;
    bool killed_ = false;
};

}  // namespace od
