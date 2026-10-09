#include "od/mm_engine.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "od/implied_vol.hpp"

namespace od {

MmConfig load_mm_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    const auto j = nlohmann::json::parse(in);
    MmConfig c;
    c.expiries = j.at("expiries").get<std::vector<std::string>>();
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
    }
    if (const auto l = j.find("limits"); l != j.end()) {
        c.limits.max_order_qty = l->value("max_order_qty", c.limits.max_order_qty);
        c.limits.max_position = l->value("max_position", c.limits.max_position);
        c.limits.max_abs_vega_usd = l->value("max_abs_vega_usd", c.limits.max_abs_vega_usd);
        c.limits.price_band_vol = l->value("price_band_vol", c.limits.price_band_vol);
        c.limits.max_orders_per_sec = l->value("max_orders_per_sec", c.limits.max_orders_per_sec);
        c.limits.burst = l->value("burst", c.limits.burst);
    }
    if (const auto h = j.find("hedge"); h != j.end()) {
        c.hedge.threshold_coin = h->value("threshold_coin", c.hedge.threshold_coin);
        c.hedge_collateral = h->value("hedge_collateral", c.hedge_collateral);
    }
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
    // Did it print at a price we were resting at, on the side it traded against?
    const auto& s = quoted_[it->second];
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
    if (first) {
        select_instruments();
        refresh_risk();
    }
}

void MarketMaker::select_instruments() {
    for (std::uint32_t id = 0; id < table_.size(); ++id) {
        const auto* q = book_.quote(id);
        if (!q || q->underlying <= 0.0) continue;
        const auto dash1 = q->name.find('-');
        const auto label = q->name.substr(dash1 + 1, q->name.find('-', dash1 + 1) - dash1 - 1);
        if (std::find(cfg_.expiries.begin(), cfg_.expiries.end(), label) == cfg_.expiries.end()) continue;
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

void MarketMaker::send(Quoted& s, Side side, std::optional<double> want, const OptionQuote& q,
                       double T, double theo_iv, double vega_per_contract) {
    std::uint64_t& oid = side == Side::Buy ? s.bid_order : s.ask_order;
    const Order* o = oid ? om_.find(oid) : nullptr;
    const bool live = o && o->live();
    if (!want) {
        if (live) gw_.cancel(oid);
        return;
    }
    if (live && std::abs(o->req.price - *want) < cfg_.min_requote_ticks * option_tick(*want) - 1e-12)
        return;  // close enough: don't churn the order (and lose queue position)

    GateContext ctx;
    ctx.position = om_.position(s.name);
    ctx.open_same_side = om_.open_qty(s.name, side) - (live ? o->req.qty - o->filled : 0.0);
    ctx.portfolio_vega_usd = risk_.vega_usd;
    ctx.vega_per_contract_usd = vega_per_contract;
    ctx.theo_iv = theo_iv;
    const auto iv = implied_vol(q.type, coin_to_usd(*want, q.underlying), q.underlying, q.strike, T);
    ctx.order_iv = iv.ok() ? iv.sigma : 0.0;

    OrderRequest req{live ? oid : om_.next_id(), s.name, side, *want, cfg_.quote.size, true};
    if (const auto why = gate_.check(req, ctx, to_ns(now_))) {
        ++stats_.rejects[*why];
        if (live && *why != "rate_limit") gw_.cancel(oid);  // don't leave a stale quote working
        return;
    }
    if (live) {
        om_.on_amend_sent(oid, *want, cfg_.quote.size);
        gw_.amend(oid, *want, cfg_.quote.size);
    } else {
        om_.on_sent(req);
        gw_.place(req);
        oid = req.client_id;
    }
}

void MarketMaker::requote() {
    for (auto& s : quoted_) {
        const auto* q = book_.quote(s.id);
        double T, theo_iv;
        if (!q || !theo_for(*q, T, theo_iv)) continue;
        const auto g = black76_greeks(q->type, q->underlying, q->strike, T, theo_iv);
        const QuoteInputs in{q->type, q->strike, q->underlying, T, theo_iv, q->bid, q->ask,
                             om_.position(s.name), risk_.vega_usd};
        const auto quote = make_quote(in, cfg_.quote);
        send(s, Side::Buy, quote.bid, *q, T, theo_iv, g.vega);
        send(s, Side::Sell, quote.ask, *q, T, theo_iv, g.vega);
    }
}

void MarketMaker::process_events() {
    events_.clear();
    gw_.poll(events_);
    for (const auto& e : events_) {
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
    if (stats_.edge_qty > 0)
        std::fprintf(out_, "average model edge at fill: %+.2f vol pts over %.1f contracts\n",
                     stats_.edge_vol_qty / stats_.edge_qty, stats_.edge_qty);
    std::fflush(out_);
}

}  // namespace od
