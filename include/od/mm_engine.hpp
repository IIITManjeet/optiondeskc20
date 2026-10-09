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

struct MmConfig {
    std::vector<std::string> expiries;
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
    void send(Quoted& s, Side side, std::optional<double> want, const OptionQuote& q, double T,
              double theo_iv, double vega_per_contract);
    void requote();
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
