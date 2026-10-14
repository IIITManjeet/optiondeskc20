#pragma once
// Market-making engine, independent of where its data and time come from.
//
// The engine never reads a clock or a socket. A driver feeds it market data
// (on_ticker / on_trade) and the current time (on_timer), and it answers through
// the Gateway. The live driver (od_mm) feeds it from the shared-memory bus and
// the steady clock; the replay driver (od_replay) feeds it from a journal with the
// recorded timestamps, so a replay is deterministic and runs as fast as the CPU
// allows. Same code, same decisions.

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "od/gateway.hpp"
#include "od/market_data.hpp"
#include "od/quoter.hpp"
#include "od/risk.hpp"
#include "od/risk_gate.hpp"

namespace od {

// How often options trade, by days to expiry x |ln(K/F)|: trades per minute per
// option, from tools/journal_stats.py --profile. Buckets are [edge_i, edge_i+1);
// the last bucket on each axis is open-ended.
struct FlowPrior {
    std::vector<double> days_edges, moneyness_edges;  // lower edges
    std::vector<std::vector<double>> rate_per_min;    // [days bucket][moneyness bucket]
    bool empty() const { return rate_per_min.empty(); }
    double rate(double days, double abs_log_moneyness) const;
};

FlowPrior load_flow_prior(const std::string& path);

struct MmConfig {
    // Which expiries to quote: by label, or (if the list is empty) every expiry
    // between min_days and max_days away, so a config doesn't go stale.
    std::vector<std::string> expiries;
    double min_days = 0.5, max_days = 10.0;
    double max_moneyness = 0.08;
    bool otm_only = true;
    double coin_balance = 2.0;
    int requote_ms = 250;
    double min_requote_ticks = 1.0;
    QuoteParams quote;
    RiskLimits limits;
    HedgeParams hedge;
    // false: hedge only the delta of options + perp, leaving the coin balance's own
    // USD exposure alone. true: run the whole account USD-neutral, collateral included.
    bool hedge_collateral = false;
    // Spend the request budget on the most valuable quote updates first (true), or
    // update instruments in a fixed order until the rate limit refuses (false).
    bool allocate_requests = true;
    // The allocator scores a quote by expected profit per minute: edge x expected
    // fills per minute. Expected fills blend the flow prior (worth prior_strength_min
    // minutes of observation) with trades seen in the instrument, never below
    // fill_rate_floor_per_min so instruments with no history still get some budget.
    FlowPrior flow_prior;
    double prior_strength_min = 10.0;
    double fill_rate_floor_per_min = 0.005;
};

MmConfig load_mm_config(const std::string& path);

using Clock = std::chrono::steady_clock;

struct MmStats {
    std::uint64_t hedges = 0;
    std::uint64_t trades_quoted = 0;    // public trades in instruments we quote
    std::uint64_t trades_at_quote = 0;  // ...that printed at one of our resting prices
    double queue_ahead_at_trade = 0.0;  // summed queue estimate at those prints
    double edge_vol_qty = 0.0, edge_qty = 0.0;  // model edge at fill (vol pts x contracts)
    std::array<double, 3> mark_vol_qty{}, mark_coin{}, mark_qty{};
    std::map<std::string, std::uint64_t> rejects;

    // Quote quality, sampled every requote cycle over every side we want to show.
    std::uint64_t sides_wanted = 0, sides_showing = 0, sides_adverse = 0;
    double off_target_vol = 0.0;   // summed over showing sides: |our price - wanted|, vol pts
    std::uint64_t deferred = 0;    // actions left for a later cycle by the allocator
    // Where the request budget went: requests for options under 2 days to expiry
    // (where most of the flow is), out of all quote requests sent.
    std::uint64_t requests = 0, requests_short_dated = 0;
    double short_dated_share() const { return requests ? double(requests_short_dated) / requests : 0.0; }
    double quotes_up() const { return sides_wanted ? double(sides_showing) / sides_wanted : 0.0; }
    double avg_off_target_vol() const { return sides_showing ? off_target_vol / sides_showing : 0.0; }
    double adverse_share() const { return sides_showing ? double(sides_adverse) / sides_showing : 0.0; }
};

class MarketMaker {
public:
    static constexpr std::array<int, 3> kMarkHorizons{1, 5, 30};  // seconds

    // `request_fit` is asked for a surface refit about once a second; the answer
    // arrives through set_view(), now (replay) or later (live, from a fit thread).
    // `sim`, if given, is fed market data so it can produce fills.
    MarketMaker(MmConfig cfg, InstrumentTable table, std::string currency, Gateway& gw,
                SimGateway* sim, std::function<void(OptionChain)> request_fit, std::FILE* out,
                int status_ms);

    void on_ticker(const TickerUpdate& u);
    void on_trade(const TradeUpdate& t);
    void set_view(std::shared_ptr<const MarketView> view);
    void on_timer(Clock::time_point now);

    void set_show_quotes(bool on) { show_quotes_ = on; }
    // Start with a position that already exists on the exchange, so risk and
    // hedging include it. Call before trading starts.
    void adopt_position(const std::string& instrument, double qty, double entry_price);
    void kill();     // cancel everything, block new orders
    void finish();   // cancel everything, print the final report

    bool ready() const { return view_ != nullptr && !quoted_.empty(); }
    std::size_t quoted() const { return quoted_.size(); }
    const MmStats& stats() const { return stats_; }
    const OrderManager& orders() const { return om_; }
    double pnl_coin() const;
    const LiveBook& book() const { return book_; }

private:
    struct Quoted {
        std::uint32_t id;
        std::string name;
        std::uint64_t bid_order = 0, ask_order = 0;
        double activity = 0.0;        // trades, exponentially decayed (half-life 5 min)
        Clock::time_point activity_at{};
    };
    // One wanted change to a quote side, scored by how much it matters (USD).
    struct Action {
        std::size_t qi;
        Side side;
        std::optional<double> want;  // nullopt = cancel
        double score;
        const OptionQuote* q;
        double T, theo_iv, vega_usd;
    };
    struct PendingMark {
        Clock::time_point due;
        std::uint32_t id;
        double side, price, qty, vega_coin;
        std::size_t h;
    };

    void select_instruments();
    bool theo_for(const OptionQuote& q, double& T, double& iv) const;
    void refresh_risk();
    double hedgeable_delta() const;
    bool execute(const Action& a);
    bool execute_impl(const Action& a);
    double activity(const Quoted& s) const;
    double expected_trades_per_min(const Quoted& s, const OptionQuote& q) const;
    void requote();
    void open_vega_same_dir(std::uint64_t self, bool self_live, double direction,
                            std::int64_t expiry_ms, double& total, double& in_expiry) const;
    void process_events();
    void settle_marks();
    void hedge();
    void status(bool final);
    void print_quotes();

    MmConfig cfg_;
    InstrumentTable table_;
    std::string ccy_;
    Gateway& gw_;
    SimGateway* sim_;
    std::function<void(OptionChain)> request_fit_;
    std::FILE* out_;
    std::chrono::milliseconds status_every_;

    LiveBook book_;
    OrderManager om_;
    RiskGate gate_;
    std::shared_ptr<const MarketView> view_;
    Portfolio risk_pf_;  // RiskReport rows point into it: keep alive alongside risk_
    RiskReport risk_;
    std::vector<Quoted> quoted_;
    std::unordered_map<std::uint32_t, std::size_t> quoted_index_;
    std::vector<PendingMark> marks_;
    std::vector<ExecEvent> events_;
    MmStats stats_;

    Clock::time_point now_{};
    bool started_ = false;
    bool show_quotes_ = false;
    Clock::time_point next_requote_{}, next_risk_{}, next_refit_{}, next_status_{};
};

}  // namespace od
