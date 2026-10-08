#pragma once
// Position and risk engine for a coin-margined (inverse) options book.
//
// On Deribit the account is held in the coin: options and the inverse perpetual
// settle in BTC, and the BTC balance itself is a long position in BTC. So there
// are two P&L views and they disagree:
//   USD view : equity_usd  = index * equity_coin   (what a USD-based trader cares about)
//   coin view: equity_coin = balance + option values in coin + perp P&L in coin
// A book can be flat in one view and carry real exposure in the other. Every
// number here is computed in both.
//
// Valuation (used for scenarios and for the portfolio-level Greeks):
//   option : Black-76 on its expiry's forward, IV from the fitted SVI smile
//            (falls back to the exchange mark IV when that expiry has no fit)
//   perp   : inverse contract, size S in USD. Coin P&L = S * (1/entry - 1/price).
//            Marked at the index (funding and the perp/index basis are ignored).
//
// Portfolio delta and gamma are bump-and-reprice on that same valuation, so they
// are consistent with the scenario grid by construction. The per-position table
// shows analytic Black-76 Greeks for intuition.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "od/surface.hpp"

namespace od {

enum class PositionKind { Option, Perpetual };

struct Position {
    std::string instrument;
    PositionKind kind = PositionKind::Option;
    double qty = 0.0;          // option: contracts (1 contract = 1 coin), negative = short
                               // perp: USD notional, negative = short
    double entry_price = 0.0;  // option: coin per contract; perp: USD
};

struct Portfolio {
    std::string currency;
    double coin_balance = 0.0;  // cash collateral in the coin
    std::vector<Position> positions;
};

struct ExpiryMarket {
    std::int64_t expiry_ms = 0;
    double forward = 0.0;
    double T = 0.0;
    std::optional<SviParams> smile;
};

// Everything valuation needs, decoupled from how it was obtained (REST snapshot or live feed).
class MarketView {
public:
    static MarketView build(const OptionChain& chain, const std::vector<SmileFit>& fits);

    double index() const { return index_; }
    std::int64_t asof_ms() const { return asof_ms_; }
    const OptionQuote* quote(const std::string& name) const;
    const ExpiryMarket* expiry(std::int64_t expiry_ms) const;

private:
    double index_ = 0.0;
    std::int64_t asof_ms_ = 0;
    std::unordered_map<std::string, OptionQuote> quotes_;
    std::map<std::int64_t, ExpiryMarket> expiries_;
};

// A market move to revalue under.
struct Shock {
    double spot = 0.0;      // relative move applied to the index and every forward (0.1 = +10%)
    double vol_pts = 0.0;   // parallel IV shift in vol points (5 = +5 pts)
    double days = 0.0;      // time decay: calendar days forward
    bool sticky_strike = false;  // default sticky-moneyness: the smile moves with the forward
};

enum class IvSource { Svi, ExchangeMark, None };
const char* to_string(IvSource s);

struct PositionRisk {
    const Position* position = nullptr;
    bool priced = false;  // false if the instrument isn't in the market view
    std::int64_t expiry_ms = 0;
    double forward = 0.0, T = 0.0;
    double iv = 0.0;
    IvSource iv_source = IvSource::None;
    double mark_coin = 0.0;   // model value per contract, in coin (perp: price in USD)
    double pnl_coin = 0.0;    // vs entry
    // Analytic Black-76, scaled by qty. delta = d(USD value)/dF in coin;
    // delta_premium_adj = F * d(coin value)/dF, the exposure of coin equity;
    // gamma_1pct = change in delta for a 1% move; vega in USD per vol point;
    // theta in USD per day.
    double delta = 0.0, delta_premium_adj = 0.0, gamma_1pct = 0.0, vega_usd = 0.0, theta_usd = 0.0;
};

struct RiskReport {
    std::vector<PositionRisk> rows;
    int unpriced = 0;
    double equity_coin = 0.0, equity_usd = 0.0;

    // Portfolio level, bump-and-reprice (includes coin balance and perp).
    double delta_usd_view = 0.0;   // coin-equivalent exposure: d(equity_usd)/d(index), in coin
    double delta_coin_view = 0.0;  // index * d(equity_coin)/d(index), in coin; sums the
                                   // per-position premium-adjusted deltas
    double gamma_usd_view = 0.0;   // change in delta_usd_view for a +1% move, in coin

    // Sums of the analytic per-position Greeks.
    double vega_usd = 0.0, theta_usd = 0.0;
    std::map<std::int64_t, double> vega_by_expiry;  // USD per vol point
};

RiskReport compute_risk(const Portfolio& pf, const MarketView& mkt);

struct Valuation {
    double equity_coin = 0.0;
    double equity_usd = 0.0;
};

Valuation value_portfolio(const Portfolio& pf, const MarketView& mkt, const Shock& shock = {});

struct ScenarioGrid {
    std::vector<double> spot;     // relative moves
    std::vector<double> vol_pts;
    double days = 0.0;
    std::vector<std::vector<double>> pnl_usd;   // [vol][spot]
    std::vector<std::vector<double>> pnl_coin;  // [vol][spot]
};

ScenarioGrid scenario_grid(const Portfolio& pf, const MarketView& mkt, std::vector<double> spot,
                           std::vector<double> vol_pts, double days = 0.0,
                           bool sticky_strike = false);

}  // namespace od
