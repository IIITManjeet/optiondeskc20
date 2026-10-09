#include "od/risk_gate.hpp"

#include <algorithm>
#include <cmath>

namespace od {

std::optional<std::string> RiskGate::check(const OrderRequest& req, const GateContext& ctx,
                                           std::int64_t now_ns) {
    if (killed_) return "kill_switch";
    if (!(req.qty > 0.0) || !(req.price > 0.0)) return "bad_order";

    if (!is_perpetual(req.instrument)) {
        if (req.qty > lim_.max_order_qty + 1e-12) return "max_order_qty";

        const double s = sign(req.side);
        const double worst_pos = ctx.position + s * (ctx.open_same_side + req.qty);
        if (std::abs(worst_pos) > lim_.max_position + 1e-12 && std::abs(worst_pos) > std::abs(ctx.position))
            return "max_position";

        const double vega_after = ctx.portfolio_vega_usd + s * req.qty * ctx.vega_per_contract_usd;
        if (std::abs(vega_after) > lim_.max_abs_vega_usd &&
            std::abs(vega_after) > std::abs(ctx.portfolio_vega_usd))
            return "max_vega";

        if (ctx.theo_iv > 0.0) {
            if (ctx.order_iv <= 0.0) return "price_unpriceable";
            if (std::abs(ctx.order_iv - ctx.theo_iv) > lim_.price_band_vol) return "price_band";
        }
    }

    // Token bucket: refill continuously, spend one token per accepted order.
    if (last_ns_ != 0)
        tokens_ = std::min(lim_.burst, tokens_ + (now_ns - last_ns_) * 1e-9 * lim_.max_orders_per_sec);
    last_ns_ = now_ns;
    if (tokens_ < 1.0) return "rate_limit";
    tokens_ -= 1.0;
    return std::nullopt;
}

}  // namespace od
