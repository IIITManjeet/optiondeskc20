#include "od/mm_engine.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "od/implied_vol.hpp"

namespace od {

double FlowPrior::rate(double days, double abs_k) const {
    if (empty()) return 0.0;
    auto bucket = [](const std::vector<double>& lower_edges, double x) {
        std::size_t i = 0;
        while (i + 1 < lower_edges.size() && x >= lower_edges[i + 1]) ++i;
        return i;
    };
    const auto d = bucket(days_edges, days);
    const auto k = bucket(moneyness_edges, abs_k);
    return d < rate_per_min.size() && k < rate_per_min[d].size() ? rate_per_min[d][k] : 0.0;
}

FlowPrior load_flow_prior(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open flow prior " + path);
    const auto j = nlohmann::json::parse(in);
    // Files written by journal_stats.py list every edge, with null for the open end.
    auto lower_edges = [](const nlohmann::json& e) {
        std::vector<double> out;
        for (const auto& x : e)
            if (x.is_number()) out.push_back(x.get<double>());
        return out;
    };
    FlowPrior p;
    p.days_edges = lower_edges(j.at("days_edges"));
    p.moneyness_edges = lower_edges(j.at("moneyness_edges"));
    p.rate_per_min = j.at("rate_per_min").get<std::vector<std::vector<double>>>();
    if (p.rate_per_min.size() != p.days_edges.size())
        throw std::runtime_error(path + ": rate_per_min rows don't match days_edges");
    for (const auto& row : p.rate_per_min)
        if (row.size() != p.moneyness_edges.size())
            throw std::runtime_error(path + ": rate_per_min columns don't match moneyness_edges");
    return p;
}

MmConfig load_mm_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    const auto j = nlohmann::json::parse(in);
    MmConfig c;
    c.expiries = j.value("expiries", std::vector<std::string>{});
    c.min_days = j.value("min_days", c.min_days);
    c.max_days = j.value("max_days", c.max_days);
    c.max_moneyness = j.value("max_moneyness", c.max_moneyness);
    c.otm_only = j.value("otm_only", c.otm_only);
    c.coin_balance = j.value("coin_balance", c.coin_balance);
    c.requote_ms = j.value("requote_ms", c.requote_ms);
    c.min_requote_ticks = j.value("min_requote_ticks", c.min_requote_ticks);
    if (const auto q = j.find("quote"); q != j.end()) {
        c.quote.half_spread_vol = q->value("half_spread_vol", c.quote.half_spread_vol);
        c.quote.skew_vol_per_1k_vega = q->value("skew_vol_per_1k_vega", c.quote.skew_vol_per_1k_vega);
        c.quote.max_skew_vol = q->value("max_skew_vol", c.quote.max_skew_vol);
        c.quote.size = q->value("size", c.quote.size);
        c.quote.max_position = q->value("max_position", c.quote.max_position);
        c.quote.max_tick_vol = q->value("max_tick_vol", c.quote.max_tick_vol);
        c.quote.min_edge_to_fee = q->value("min_edge_to_fee", c.quote.min_edge_to_fee);
    }
    if (const auto l = j.find("limits"); l != j.end()) {
        c.limits.max_order_qty = l->value("max_order_qty", c.limits.max_order_qty);
        c.limits.max_position = l->value("max_position", c.limits.max_position);
        c.limits.max_abs_vega_usd = l->value("max_abs_vega_usd", c.limits.max_abs_vega_usd);
        c.limits.max_abs_vega_per_expiry_usd =
            l->value("max_abs_vega_per_expiry_usd", c.limits.max_abs_vega_per_expiry_usd);
        c.limits.price_band_vol = l->value("price_band_vol", c.limits.price_band_vol);
        c.limits.max_orders_per_sec = l->value("max_orders_per_sec", c.limits.max_orders_per_sec);
        c.limits.burst = l->value("burst", c.limits.burst);
    }
    if (const auto h = j.find("hedge"); h != j.end()) {
        c.hedge.threshold_coin = h->value("threshold_coin", c.hedge.threshold_coin);
        c.hedge_collateral = h->value("hedge_collateral", c.hedge_collateral);
    }
    c.allocate_requests = j.value("allocate_requests", c.allocate_requests);
    if (const auto fp = j.find("flow_prior"); fp != j.end() && fp->is_string()) {
        // Relative paths are relative to the config file.
        std::filesystem::path p = fp->get<std::string>();
        if (p.is_relative()) p = std::filesystem::path(path).parent_path() / p;
        c.flow_prior = load_flow_prior(p.string());
    }
    c.prior_strength_min = j.value("prior_strength_min", c.prior_strength_min);
    c.fill_rate_floor_per_min = j.value("fill_rate_floor_per_min", c.fill_rate_floor_per_min);
    return c;
}

namespace {
std::string hms(std::int64_t ms) {
    const std::time_t t = ms / 1000;
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::gmtime(&t));
    return buf;
}
std::int64_t to_ns(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}
}  // namespace

MarketMaker::MarketMaker(MmConfig cfg, InstrumentTable table, std::string currency, Gateway& gw,
                         SimGateway* sim, std::function<void(OptionChain)> request_fit,
                         std::FILE* out, int status_ms)
    : cfg_(std::move(cfg)),
      table_(std::move(table)),
      ccy_(std::move(currency)),
      gw_(gw),
      sim_(sim),
      request_fit_(std::move(request_fit)),
      out_(out),
      status_every_(status_ms),
      book_(table_),
      om_(ccy_, cfg_.coin_balance),
      gate_(cfg_.limits) {}

void MarketMaker::on_ticker(const TickerUpdate& u) {
    book_.apply(u);
    if (sim_ && quoted_index_.contains(u.instrument))
        sim_->on_market(table_[u.instrument].name, u.bid, u.ask, u.bid_amount, u.ask_amount);
}

void MarketMaker::on_trade(const TradeUpdate& t) {
    const auto it = quoted_index_.find(t.instrument);
    if (it == quoted_index_.end()) return;
    ++stats_.trades_quoted;
    auto& s = quoted_[it->second];
    s.activity = activity(s) + 1.0;
    s.activity_at = now_;
    // Did it print at a price we were resting at, on the side it traded against?
    const std::uint64_t oid = t.taker_buy ? s.ask_order : s.bid_order;
    if (const auto* o = oid ? om_.find(oid) : nullptr; o && o->live() && std::abs(o->req.price - t.price) < 1e-9) {
        ++stats_.trades_at_quote;
        if (sim_) {
            const double q = sim_->queue_ahead(oid);
            if (std::isfinite(q)) stats_.queue_ahead_at_trade += q;
        }
    }
    if (sim_) sim_->on_trade(table_[t.instrument].name, t.price, t.amount, t.taker_buy != 0);
}

void MarketMaker::set_view(std::shared_ptr<const MarketView> view) {
    if (!view) return;
    const bool first = view_ == nullptr;
    view_ = std::move(view);
    // Pick what to quote from the book as it is now; retry on later surfaces if the
    // book wasn't populated enough yet to match anything.
    if (quoted_.empty()) select_instruments();
    if (first) refresh_risk();
}

void MarketMaker::adopt_position(const std::string& instrument, double qty, double entry_price) {
    om_.adopt(instrument, qty, entry_price);
    if (view_) refresh_risk();
}

void MarketMaker::select_instruments() {
    for (std::uint32_t id = 0; id < table_.size(); ++id) {
        const auto* q = book_.quote(id);
        if (!q || q->underlying <= 0.0) continue;
        const auto dash1 = q->name.find('-');
        const auto label = q->name.substr(dash1 + 1, q->name.find('-', dash1 + 1) - dash1 - 1);
        if (cfg_.expiries.empty()) {
            const double days = static_cast<double>(q->expiry_ms - book_.last_exch_ts_ms()) / 86'400'000.0;
            if (days < cfg_.min_days || days > cfg_.max_days) continue;
        } else if (std::find(cfg_.expiries.begin(), cfg_.expiries.end(), label) == cfg_.expiries.end()) {
            continue;
        }
        const double k = std::log(q->strike / q->underlying);
        if (std::abs(k) > cfg_.max_moneyness) continue;
        if (cfg_.otm_only && (q->type == OptionType::Call ? k < 0 : k >= 0)) continue;
        quoted_index_[id] = quoted_.size();
        quoted_.push_back({id, q->name});
    }
}

bool MarketMaker::theo_for(const OptionQuote& q, double& T, double& iv) const {
    const auto* em = view_->expiry(q.expiry_ms);
    T = static_cast<double>(q.expiry_ms - book_.last_exch_ts_ms()) / kMsPerYear;
    iv = 0.0;
    if (em && em->smile) iv = em->smile->iv(std::log(q.strike / q.underlying), em->T);
    if (iv <= 0.0) iv = q.mark_iv;
    return T > 0.0 && iv > 0.0 && q.underlying > 0.0;
}

void MarketMaker::refresh_risk() {
    risk_pf_ = om_.portfolio();
    risk_ = compute_risk(risk_pf_, *view_);
}

// USD delta the hedger targets (coin): optionally excluding the coin balance,
// whose USD delta is exactly its size.
double MarketMaker::hedgeable_delta() const {
    return risk_.delta_usd_view - (cfg_.hedge_collateral ? 0.0 : risk_pf_.coin_balance);
}

double MarketMaker::pnl_coin() const {
    if (!view_) return 0.0;
    return value_portfolio(om_.portfolio(), *view_).equity_coin - cfg_.coin_balance;
}

// Carry out one planned action. Returns true if a request went to the gateway.
bool MarketMaker::execute(const Action& a) {
    const bool sent = execute_impl(a);
    if (sent) {
        ++stats_.requests;
        if (static_cast<double>(a.q->expiry_ms - book_.last_exch_ts_ms()) < 2.0 * 86'400'000.0)
            ++stats_.requests_short_dated;
    }
    return sent;
}

bool MarketMaker::execute_impl(const Action& a) {
    Quoted& s = quoted_[a.qi];
    const OptionQuote& q = *a.q;
    std::uint64_t& oid = a.side == Side::Buy ? s.bid_order : s.ask_order;
    const Order* o = oid ? om_.find(oid) : nullptr;
    const bool live = o && o->live();
    if (!a.want) {
        if (!live) return false;
        gate_.spend(to_ns(now_));
        gw_.cancel(oid);
        return true;
    }

    GateContext ctx;
    ctx.position = om_.position(s.name);
    ctx.open_same_side = om_.open_qty(s.name, a.side) - (live ? o->req.qty - o->filled : 0.0);
    ctx.portfolio_vega_usd = risk_.vega_usd;
    if (const auto it = risk_.vega_by_expiry.find(q.expiry_ms); it != risk_.vega_by_expiry.end())
        ctx.expiry_vega_usd = it->second;
    ctx.vega_per_contract_usd = a.vega_usd;
    open_vega_same_dir(oid, live, sign(a.side) * a.vega_usd, q.expiry_ms, ctx.open_vega_same_dir_usd,
                       ctx.open_expiry_vega_same_dir_usd);
    ctx.theo_iv = a.theo_iv;
    const auto iv = implied_vol(q.type, coin_to_usd(*a.want, q.underlying), q.underlying, q.strike, a.T);
    ctx.order_iv = iv.ok() ? iv.sigma : 0.0;

    OrderRequest req{live ? oid : om_.next_id(), s.name, a.side, *a.want, cfg_.quote.size, true};
    if (const auto why = gate_.check(req, ctx, to_ns(now_))) {
        ++stats_.rejects[*why];
        if (live && *why != "rate_limit") {  // don't leave a stale quote working
            gate_.spend(to_ns(now_));
            gw_.cancel(oid);
            return true;
        }
        return false;
    }
    if (live) {
        om_.on_amend_sent(oid, *a.want, cfg_.quote.size);
        gw_.amend(oid, *a.want, cfg_.quote.size);
    } else {
        om_.on_sent(req);
        gw_.place(req);
        oid = req.client_id;
    }
    return true;
}

// Expected public trades per minute in this option: the flow prior, worth
// prior_strength_min minutes of observation, blended with the trades actually seen
// (a decayed count whose effective window is half_life / ln 2 = ~7.2 minutes).
double MarketMaker::expected_trades_per_min(const Quoted& s, const OptionQuote& q) const {
    constexpr double kWindowMin = 5.0 / 0.6931471805599453;
    const double days = static_cast<double>(q.expiry_ms - book_.last_exch_ts_ms()) / 86'400'000.0;
    const double prior = cfg_.flow_prior.rate(days, std::abs(std::log(q.strike / q.underlying)));
    const double m = cfg_.prior_strength_min;
    const double rate = (prior * m + activity(s)) / (m + kWindowMin);
    return std::max(rate, cfg_.fill_rate_floor_per_min);
}

double MarketMaker::activity(const Quoted& s) const {
    if (s.activity <= 0.0) return 0.0;
    const double dt = std::chrono::duration<double>(now_ - s.activity_at).count();
    return s.activity * std::exp2(-dt / 300.0);  // half-life 5 minutes
}

// Signed vega of our other live option orders that push vega the same way as
// `direction`, in total and within `expiry_ms`: the worst case if they all fill.
void MarketMaker::open_vega_same_dir(std::uint64_t self, bool self_live, double direction,
                                     std::int64_t expiry_ms, double& total, double& in_expiry) const {
    total = in_expiry = 0.0;
    for (const auto* o : om_.live_orders()) {
        if ((self_live && o->req.client_id == self) || is_perpetual(o->req.instrument)) continue;
        const auto id = table_.find(o->req.instrument);
        const auto* q = id ? book_.quote(*id) : nullptr;
        double T, iv;
        if (!q || !theo_for(*q, T, iv)) continue;
        const double v = sign(o->req.side) * (o->req.qty - o->filled) *
                         black76_greeks(q->type, q->underlying, q->strike, T, iv).vega;
        if (v * direction <= 0.0) continue;
        total += v;
        if (q->expiry_ms == expiry_ms) in_expiry += v;
    }
}

// Plan every wanted change to our quotes, then spend the request budget on them.
//
// With a rate limit of a few requests per second we can't update every quote that
// moves, so each action gets a score (USD per minute) and the budget goes to the
// highest:
//   cancel : always first. A quote we don't want any more is pure risk.
//   amend  : |resting price - wanted price| x size x index. An order that has become
//            too generous (bid too high / offer too low: the side that gets picked
//            off) counts 3x; one that is merely too passive is a missed opportunity.
//   place  : the edge it would earn, |theo - wanted price| x size x index.
// Each is multiplied by the expected trades per minute in the option (flow prior
// blended with observed trades), so a quote is valued by what it can actually earn
// or lose per minute: big edge on an option that never trades is worth little.
void MarketMaker::requote() {
    constexpr double kCancel = 1e12;
    const double index = view_->index();
    std::vector<Action> plan;
    for (std::size_t i = 0; i < quoted_.size(); ++i) {
        auto& s = quoted_[i];
        const auto* q = book_.quote(s.id);
        double T, theo_iv;
        if (!q || !theo_for(*q, T, theo_iv)) continue;
        const auto g = black76_greeks(q->type, q->underlying, q->strike, T, theo_iv);
        const double vega_coin = usd_to_coin(g.vega, q->underlying);  // per vol point
        const QuoteInputs in{q->type, q->strike, q->underlying, T, theo_iv, q->bid, q->ask,
                             om_.position(s.name), risk_.vega_usd};
        const auto quote = make_quote(in, cfg_.quote);
        const double fills_per_min = expected_trades_per_min(s, *q);

        for (const Side side : {Side::Buy, Side::Sell}) {
            const auto want = side == Side::Buy ? quote.bid : quote.ask;
            const std::uint64_t oid = side == Side::Buy ? s.bid_order : s.ask_order;
            const Order* o = oid ? om_.find(oid) : nullptr;
            const bool live = o && o->live();

            if (want) {  // quote quality, sampled before acting
                ++stats_.sides_wanted;
                if (live) {
                    ++stats_.sides_showing;
                    if (vega_coin > 0.0) stats_.off_target_vol += std::abs(o->req.price - *want) / vega_coin;
                    if (sign(side) * (o->req.price - *want) > 0.5 * option_tick(*want)) ++stats_.sides_adverse;
                }
            }

            if (!want) {
                if (live) plan.push_back({i, side, std::nullopt, kCancel, q, T, theo_iv, g.vega});
                continue;
            }
            if (live && std::abs(o->req.price - *want) < cfg_.min_requote_ticks * option_tick(*want) - 1e-12)
                continue;  // close enough: don't churn the order (and lose queue position)
            double score;
            if (live) {
                const double gap = o->req.price - *want;
                score = std::abs(gap) * cfg_.quote.size * index * (sign(side) * gap > 0.0 ? 3.0 : 1.0);
            } else {
                score = std::abs(quote.theo_coin - *want) * cfg_.quote.size * index;
            }
            plan.push_back({i, side, want, score * fills_per_min, q, T, theo_iv, g.vega});
        }
    }

    if (!cfg_.allocate_requests) {  // fixed order: whatever the rate limit lets through
        for (const auto& a : plan) execute(a);
        return;
    }
    std::stable_sort(plan.begin(), plan.end(),
                     [](const Action& a, const Action& b) { return a.score > b.score; });
    int budget = gate_.available(to_ns(now_));
    for (const auto& a : plan) {
        if (!a.want) {  // cancels always go; they still spend a token
            execute(a);
            continue;
        }
        if (budget <= 0) {
            ++stats_.deferred;
            continue;
        }
        if (execute(a)) --budget;
    }
}

void MarketMaker::process_events() {
    events_.clear();
    gw_.poll(events_);
    bool filled = false;
    for (const auto& e : events_) {
        filled |= e.kind == ExecEvent::Kind::Fill;
        om_.apply(e);
        if (e.kind == ExecEvent::Kind::Rejected) ++stats_.rejects["exch:" + e.reason];
        if (e.kind != ExecEvent::Kind::Fill) continue;
        const auto* o = om_.find(e.client_id);
        if (!o || is_perpetual(o->req.instrument)) continue;
        // Model edge at the moment of the fill, in vol points: positive = we bought
        // below / sold above our own theo.
        const auto id = table_.find(o->req.instrument);
        const auto* q = id ? book_.quote(*id) : nullptr;
        double T, theo_iv;
        if (!q || !view_ || !theo_for(*q, T, theo_iv)) continue;
        const auto g = black76_greeks(q->type, q->underlying, q->strike, T, theo_iv);
        const double theo = usd_to_coin(g.price, q->underlying);
        const double vega_coin = usd_to_coin(g.vega, q->underlying);
        if (vega_coin <= 0.0) continue;
        const double edge = sign(o->req.side) * (theo - e.price) / vega_coin;
        stats_.edge_vol_qty += edge * e.qty;
        stats_.edge_qty += e.qty;
        for (std::size_t h = 0; h < kMarkHorizons.size(); ++h)
            marks_.push_back({now_ + std::chrono::seconds(kMarkHorizons[h]), *id, sign(o->req.side),
                              e.price, e.qty, vega_coin, h});
        std::fprintf(out_, "%s FILL %-4s %4.1f %-22s @ %.4f  theo %.4f  edge %+.2f vol pts  pos %+.1f\n",
                     hms(book_.last_exch_ts_ms()).c_str(), to_string(o->req.side), e.qty,
                     o->req.instrument.c_str(), e.price, theo, edge, om_.position(o->req.instrument));
    }
    // Limits must see the position we have now, not the one from the last 1 s refresh.
    if (filled && view_) refresh_risk();
}

// Mark-outs: each fill against the market mid 1 s, 5 s and 30 s later. Edge at fill
// is what the model thought; the mark-out is what the market did next.
void MarketMaker::settle_marks() {
    std::erase_if(marks_, [&](const PendingMark& m) {
        if (now_ < m.due) return false;
        const auto* q = book_.quote(m.id);
        if (!q || q->bid <= 0.0 || q->ask <= 0.0) return true;  // no two-sided market: skip
        const double move = m.side * (0.5 * (q->bid + q->ask) - m.price);
        stats_.mark_coin[m.h] += move * m.qty;
        stats_.mark_vol_qty[m.h] += move / m.vega_coin * m.qty;
        stats_.mark_qty[m.h] += m.qty;
        return true;
    });
}

void MarketMaker::hedge() {
    refresh_risk();
    const double usd = hedge_notional_usd(hedgeable_delta(), view_->index(), cfg_.hedge);
    if (usd == 0.0) return;
    OrderRequest h{om_.next_id(), ccy_ + "-PERPETUAL", usd > 0 ? Side::Buy : Side::Sell,
                   view_->index(), std::abs(usd), false};
    if (const auto why = gate_.check(h, {}, to_ns(now_))) {
        ++stats_.rejects["hedge:" + *why];
        return;
    }
    om_.on_sent(h);
    gw_.place(h);
    ++stats_.hedges;
    process_events();
    refresh_risk();
}

void MarketMaker::on_timer(Clock::time_point now) {
    now_ = now;
    if (!started_) {
        started_ = true;
        next_requote_ = next_risk_ = next_refit_ = now;
        next_status_ = now + status_every_;
    }
    process_events();
    settle_marks();

    if (now >= next_refit_) {
        request_fit_(book_.to_chain(ccy_));
        next_refit_ = now + std::chrono::seconds(1);
    }
    if (!ready()) return;
    if (now >= next_risk_) {
        hedge();
        next_risk_ = now + std::chrono::seconds(1);
    }
    if (now >= next_requote_ && !gate_.killed()) {
        requote();
        process_events();
        next_requote_ = now + std::chrono::milliseconds(cfg_.requote_ms);
    }
    if (status_every_.count() > 0 && now >= next_status_) {
        status(false);
        next_status_ = now + status_every_;
    }
}

void MarketMaker::kill() {
    if (gate_.killed()) return;
    gate_.kill();
    gw_.cancel_all();
    std::fprintf(out_, "KILL SWITCH: all orders cancelled, new orders blocked\n");
}

void MarketMaker::status(bool final) {
    const auto pf = om_.portfolio();
    const double pnl = view_ ? value_portfolio(pf, *view_).equity_coin - cfg_.coin_balance : 0.0;
    int n_pos = 0;
    for (const auto& p : pf.positions) n_pos += !is_perpetual(p.instrument);
    std::fprintf(out_,
                 "%s %s  orders %zu  fills %llu  vol %.1f  pos %d  delta %+.3f  vega $%+.0f  "
                 "pnl %+.5f %s ($%+.0f)  fees %.5f  hedges %llu%s\n",
                 hms(book_.last_exch_ts_ms()).c_str(), final ? "FINAL " : "status",
                 om_.live_orders().size(), static_cast<unsigned long long>(om_.fills()), om_.volume(),
                 n_pos, hedgeable_delta(), risk_.vega_usd, pnl, ccy_.c_str(),
                 view_ ? pnl * view_->index() : 0.0, om_.fees_paid(),
                 static_cast<unsigned long long>(stats_.hedges), gate_.killed() ? "  KILLED" : "");
    std::fprintf(out_, "         trades in quoted instruments %llu, at our price %llu",
                 static_cast<unsigned long long>(stats_.trades_quoted),
                 static_cast<unsigned long long>(stats_.trades_at_quote));
    if (stats_.trades_at_quote > 0)
        std::fprintf(out_, " (avg queue ahead %.1f contracts)",
                     stats_.queue_ahead_at_trade / static_cast<double>(stats_.trades_at_quote));
    std::fprintf(out_, "\n");
    std::fprintf(out_, "         quotes up %.0f%%, avg off target %.2f vol pts, too generous %.0f%%, deferred %llu\n",
                 100 * stats_.quotes_up(), stats_.avg_off_target_vol(), 100 * stats_.adverse_share(),
                 static_cast<unsigned long long>(stats_.deferred));
    if (stats_.mark_qty[0] > 0.0) {
        std::fprintf(out_, "         mark-outs vs mid:");
        for (std::size_t h = 0; h < kMarkHorizons.size(); ++h)
            if (stats_.mark_qty[h] > 0.0)
                std::fprintf(out_, "  %2ds %+.2f vol pts (%+.5f %s)", kMarkHorizons[h],
                             stats_.mark_vol_qty[h] / stats_.mark_qty[h], stats_.mark_coin[h],
                             ccy_.c_str());
        std::fprintf(out_, "\n");
    }
    if (show_quotes_ && !final) print_quotes();
    if (!stats_.rejects.empty()) {
        std::fprintf(out_, "         rejects:");
        for (const auto& [why, n] : stats_.rejects)
            std::fprintf(out_, " %s=%llu", why.c_str(), static_cast<unsigned long long>(n));
        std::fprintf(out_, "\n");
    }
    std::fflush(out_);
}

// Where our quotes sit against the market, in price and in vol.
void MarketMaker::print_quotes() {
    std::fprintf(out_, "  %-22s %8s %8s | %8s %8s | %7s | %6s %6s %6s %6s\n", "instrument", "mkt_bid",
                 "mkt_ask", "our_bid", "our_ask", "theo", "mb_iv", "ob_iv", "oa_iv", "ma_iv");
    for (const auto& s : quoted_) {
        const auto* q = book_.quote(s.id);
        double T, theo_iv;
        if (!q || !theo_for(*q, T, theo_iv)) continue;
        auto iv_of = [&](double px) {
            if (px <= 0.0) return 0.0;
            const auto r = implied_vol(q->type, coin_to_usd(px, q->underlying), q->underlying, q->strike, T);
            return r.ok() ? 100 * r.sigma : 0.0;
        };
        const auto* b = s.bid_order ? om_.find(s.bid_order) : nullptr;
        const auto* a = s.ask_order ? om_.find(s.ask_order) : nullptr;
        const double ob = b && b->live() ? b->req.price : 0.0;
        const double oa = a && a->live() ? a->req.price : 0.0;
        const double theo =
            usd_to_coin(black76_price(q->type, q->underlying, q->strike, T, theo_iv), q->underlying);
        std::fprintf(out_, "  %-22s %8.4f %8.4f | %8.4f %8.4f | %7.4f | %6.2f %6.2f %6.2f %6.2f  theo_iv %.2f\n",
                     s.name.c_str(), q->bid, q->ask, ob, oa, theo, iv_of(q->bid), iv_of(ob), iv_of(oa),
                     iv_of(q->ask), 100 * theo_iv);
    }
}

void MarketMaker::finish() {
    gw_.cancel_all();
    process_events();
    if (!view_) return;
    refresh_risk();
    status(true);
    std::fprintf(out_, "\npositions:\n");
    for (const auto& row : risk_.rows) {
        if (!row.priced) continue;
        std::fprintf(out_, "  %-22s %+10.2f  delta %+7.3f  vega $%+7.0f\n",
                     row.position->instrument.c_str(), row.position->qty, row.delta, row.vega_usd);
    }
    if (!risk_.vega_by_expiry.empty()) {
        std::fprintf(out_, "vega by expiry ($ per vol pt):");
        for (const auto& [exp, v] : risk_.vega_by_expiry) {
            const std::time_t t = exp / 1000;
            char label[16];
            std::strftime(label, sizeof label, "%d%b%y", std::gmtime(&t));
            std::fprintf(out_, "  %s %+.0f", label, v);
        }
        std::fprintf(out_, "\n");
    }
    if (stats_.edge_qty > 0)
        std::fprintf(out_, "average model edge at fill: %+.2f vol pts over %.1f contracts\n",
                     stats_.edge_vol_qty / stats_.edge_qty, stats_.edge_qty);
    std::fflush(out_);
}

}  // namespace od
