// od_mm: options market maker, paper trading against live market data.
//
//   od_feedd --currency BTC                                    (terminal 1)
//   od_mm --bus od_feed_BTC --config examples/mm_btc.json      (terminal 2)
//
// Loop on the strategy thread:
//   bus updates -> LiveBook; quoted instruments also go to the simulated exchange
//   execution reports -> OrderManager (positions, cash, fees)
//   every requote_ms: theo vol from the latest SVI fit at the *current* forward,
//     quote in vol space with inventory skew, risk-gate, place/amend/cancel
//   every second: portfolio risk on the M3 engine, delta hedge with the perp
// SVI refits run on a separate thread (latest-wins), as in od_live.
//
// Ctrl-C cancels everything and prints a summary. SIGUSR1 trips the kill switch:
// cancel all, reject every new order, keep reporting.

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "od/feed_bus.hpp"
#include "od/gateway.hpp"
#include "od/implied_vol.hpp"
#include "od/quoter.hpp"
#include "od/risk.hpp"
#include "od/risk_gate.hpp"
#include "od/surface.hpp"
#include "od/thread_util.hpp"

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_kill{false};
void on_signal(int sig) { (sig == SIGUSR1 ? g_kill : g_stop).store(true); }

struct Config {
    std::vector<std::string> expiries;
    double max_moneyness = 0.08;
    bool otm_only = true;
    double coin_balance = 2.0;
    int requote_ms = 250;
    double min_requote_ticks = 1.0;
    od::QuoteParams quote;
    od::RiskLimits limits;
    od::HedgeParams hedge;
    // false: hedge only the delta of options + perp, leaving the coin balance's own
    // USD exposure alone. true: run the whole account USD-neutral, collateral included.
    bool hedge_collateral = false;
};

Config load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    const auto j = nlohmann::json::parse(in);
    Config c;
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

struct Args {
    std::string bus = "od_feed_BTC";
    std::string config = "examples/mm_btc.json";
    int duration_s = 0;
    int status_ms = 5000;
    bool show_quotes = false;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        if (f == "--show-quotes") {
            a.show_quotes = true;
            continue;
        }
        const char* v = i + 1 < argc ? argv[++i] : nullptr;
        if (!v) return false;
        if (f == "--bus") a.bus = v;
        else if (f == "--config") a.config = v;
        else if (f == "--duration") a.duration_s = std::atoi(v);
        else if (f == "--status-ms") a.status_ms = std::atoi(v);
        else return false;
    }
    return true;
}

std::string hms(std::int64_t ms) {
    const std::time_t t = ms / 1000;
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::gmtime(&t));
    return buf;
}

// Refits the surface off the strategy thread and publishes an immutable MarketView.
class Fitter {
public:
    Fitter() : thread_([this] { loop(); }) {}
    ~Fitter() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }
    void post(od::OptionChain chain) {
        {
            std::lock_guard lk(m_);
            pending_ = std::move(chain);
        }
        cv_.notify_one();
    }
    std::shared_ptr<const od::MarketView> latest() const { return view_.load(); }

private:
    void loop() {
        od::name_current_thread("od-fitter");
        for (;;) {
            od::OptionChain chain;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return stop_ || pending_; });
                if (stop_) return;
                chain = std::move(*pending_);
                pending_.reset();
            }
            const auto fits = od::build_surface(chain);
            view_.store(std::make_shared<const od::MarketView>(od::MarketView::build(chain, fits)));
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::optional<od::OptionChain> pending_;
    bool stop_ = false;
    std::atomic<std::shared_ptr<const od::MarketView>> view_;
    std::thread thread_;  // last
};

struct Quoted {
    std::uint32_t id;
    std::string name;
    std::uint64_t bid_order = 0, ask_order = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::puts("usage: od_mm [--bus SHM_NAME] [--config FILE] [--duration S] [--status-ms N]");
        return 2;
    }
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);

    Config cfg;
    std::optional<od::bus::FeedBusReader> reader;
    try {
        cfg = load_config(args.config);
        reader.emplace(od::bus::FeedBusReader::open(args.bus));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    const auto table = reader->instruments();
    const auto ccy = reader->currency();
    od::LiveBook book(table);
    for (const auto& u : reader->snapshot()) book.apply(u);

    od::name_current_thread("od-strategy");
    Fitter fitter;
    od::SimGateway gw;
    od::OrderManager om(ccy, cfg.coin_balance);
    od::RiskGate gate(cfg.limits);
    std::map<std::string, std::uint64_t> rejects;

    // Wait for the first surface, then pick what to quote.
    fitter.post(book.to_chain(ccy));
    std::shared_ptr<const od::MarketView> view;
    while (!(view = fitter.latest()) && !g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!view) return 0;

    std::vector<Quoted> quoted;
    std::unordered_map<std::uint32_t, std::size_t> quoted_index;
    for (std::uint32_t id = 0; id < table.size(); ++id) {
        const auto* q = book.quote(id);
        if (!q || q->underlying <= 0.0) continue;
        const auto id_parts = od::parse_option_name(q->name);
        if (!id_parts) continue;
        const auto dash1 = q->name.find('-');
        const auto label = q->name.substr(dash1 + 1, q->name.find('-', dash1 + 1) - dash1 - 1);
        if (std::find(cfg.expiries.begin(), cfg.expiries.end(), label) == cfg.expiries.end()) continue;
        const double k = std::log(q->strike / q->underlying);
        if (std::abs(k) > cfg.max_moneyness) continue;
        if (cfg.otm_only && (q->type == od::OptionType::Call ? k < 0 : k >= 0)) continue;
        quoted_index[id] = quoted.size();
        quoted.push_back({id, q->name});
    }
    std::fprintf(stderr, "od_mm: paper trading %zu %s options on the simulated exchange\n",
                 quoted.size(), ccy.c_str());
    if (quoted.empty()) {
        std::fprintf(stderr, "nothing to quote: check \"expiries\" in %s\n", args.config.c_str());
        return 1;
    }

    // RiskReport rows point into the Portfolio they were computed from, so keep the
    // portfolio alive alongside the report.
    od::Portfolio risk_pf;
    od::RiskReport risk;
    auto refresh_risk = [&] {
        risk_pf = om.portfolio();
        risk = od::compute_risk(risk_pf, *view);
    };
    // USD delta the hedger targets (coin): optionally excluding the coin balance,
    // whose USD delta is exactly its size.
    auto hedgeable_delta = [&] {
        return risk.delta_usd_view - (cfg.hedge_collateral ? 0.0 : risk_pf.coin_balance);
    };
    refresh_risk();
    const double start_equity_coin = cfg.coin_balance;
    double edge_sum_vol = 0.0;  // model edge at fill, in vol points, summed over contracts
    double edge_qty = 0.0;
    std::uint64_t hedges = 0;

    auto theo_for = [&](const od::OptionQuote& q, double& T, double& iv) {
        const auto* em = view->expiry(q.expiry_ms);
        T = static_cast<double>(q.expiry_ms - book.last_exch_ts_ms()) / od::kMsPerYear;
        iv = 0.0;
        if (em && em->smile) iv = em->smile->iv(std::log(q.strike / q.underlying), em->T);
        if (iv <= 0.0) iv = q.mark_iv;
        return T > 0.0 && iv > 0.0 && q.underlying > 0.0;
    };

    auto send = [&](Quoted& s, od::Side side, std::optional<double> want, const od::OptionQuote& q,
                    double T, double theo_iv, double vega_per_contract) {
        std::uint64_t& oid = side == od::Side::Buy ? s.bid_order : s.ask_order;
        const od::Order* o = oid ? om.find(oid) : nullptr;
        const bool live = o && o->live();
        if (!want) {
            if (live) gw.cancel(oid);
            return;
        }
        if (live && std::abs(o->req.price - *want) < cfg.min_requote_ticks * od::option_tick(*want) - 1e-12)
            return;  // close enough: don't churn the order

        od::GateContext ctx;
        ctx.position = om.position(s.name);
        ctx.open_same_side = om.open_qty(s.name, side) - (live ? o->req.qty - o->filled : 0.0);
        ctx.portfolio_vega_usd = risk.vega_usd;
        ctx.vega_per_contract_usd = vega_per_contract;
        ctx.theo_iv = theo_iv;
        const auto iv = od::implied_vol(q.type, od::coin_to_usd(*want, q.underlying), q.underlying,
                                        q.strike, T);
        ctx.order_iv = iv.ok() ? iv.sigma : 0.0;

        od::OrderRequest req{live ? oid : om.next_id(), s.name, side, *want, cfg.quote.size, true};
        if (const auto why = gate.check(req, ctx, od::now_ns())) {
            ++rejects[*why];
            if (live && *why != "rate_limit") gw.cancel(oid);  // don't leave a stale quote working
            return;
        }
        if (live) {
            om.on_amend_sent(oid, *want, cfg.quote.size);
            gw.amend(oid, *want, cfg.quote.size);
        } else {
            om.on_sent(req);
            gw.place(req);
            oid = req.client_id;
        }
    };

    auto requote = [&] {
        for (auto& s : quoted) {
            const auto* q = book.quote(s.id);
            double T, theo_iv;
            if (!q || !theo_for(*q, T, theo_iv)) continue;
            const auto g = od::black76_greeks(q->type, q->underlying, q->strike, T, theo_iv);
            od::QuoteInputs in{q->type, q->strike, q->underlying, T, theo_iv, q->bid, q->ask,
                               om.position(s.name), risk.vega_usd};
            const auto quote = od::make_quote(in, cfg.quote);
            send(s, od::Side::Buy, quote.bid, *q, T, theo_iv, g.vega);
            send(s, od::Side::Sell, quote.ask, *q, T, theo_iv, g.vega);
        }
    };

    std::vector<od::ExecEvent> events;
    auto process_events = [&] {
        events.clear();
        gw.poll(events);
        for (const auto& e : events) {
            om.apply(e);
            if (e.kind == od::ExecEvent::Kind::Rejected) ++rejects["exch:" + e.reason];
            if (e.kind != od::ExecEvent::Kind::Fill) continue;
            const auto* o = om.find(e.client_id);
            if (!o || od::is_perpetual(o->req.instrument)) continue;
            // Model edge at the moment of the fill, in vol points: positive = we bought
            // below / sold above our own theo. Fills that only happen when the market
            // trades through us tend to show negative edge: adverse selection.
            const auto id = table.find(o->req.instrument);
            const auto* q = id ? book.quote(*id) : nullptr;
            double T, theo_iv;
            if (q && theo_for(*q, T, theo_iv)) {
                const auto g = od::black76_greeks(q->type, q->underlying, q->strike, T, theo_iv);
                const double theo = od::usd_to_coin(g.price, q->underlying);
                const double vega_coin = od::usd_to_coin(g.vega, q->underlying);
                if (vega_coin > 0.0) {
                    const double edge = od::sign(o->req.side) * (theo - e.price) / vega_coin;
                    edge_sum_vol += edge * e.qty;
                    edge_qty += e.qty;
                    std::printf("%s FILL %-4s %4.1f %-22s @ %.4f  theo %.4f  edge %+.2f vol pts  pos %+.1f\n",
                                hms(book.last_exch_ts_ms()).c_str(), od::to_string(o->req.side), e.qty,
                                o->req.instrument.c_str(), e.price, theo, edge,
                                om.position(o->req.instrument));
                }
            }
        }
    };

    auto status = [&](bool final) {
        const auto pf = om.portfolio();
        const auto val = od::value_portfolio(pf, *view);
        const double pnl_coin = val.equity_coin - start_equity_coin;
        int n_pos = 0;
        for (const auto& p : pf.positions) n_pos += !od::is_perpetual(p.instrument);
        std::printf("%s %s  orders %zu  fills %llu  vol %.1f  pos %d  delta %+.3f  vega $%+.0f  "
                    "pnl %+.5f %s ($%+.0f)  fees %.5f  hedges %llu%s\n",
                    hms(book.last_exch_ts_ms()).c_str(), final ? "FINAL " : "status", om.live_orders().size(),
                    static_cast<unsigned long long>(om.fills()), om.volume(), n_pos, hedgeable_delta(),
                    risk.vega_usd, pnl_coin, ccy.c_str(), pnl_coin * view->index(), om.fees_paid(),
                    static_cast<unsigned long long>(hedges), gate.killed() ? "  KILLED" : "");
        if (!rejects.empty()) {
            std::printf("         rejects:");
            for (const auto& [why, n] : rejects) std::printf(" %s=%llu", why.c_str(), static_cast<unsigned long long>(n));
            std::printf("\n");
        }
        if (args.show_quotes && !final) {
            // Where our quotes sit against the market, in price and in vol.
            std::printf("  %-22s %8s %8s | %8s %8s | %7s | %6s %6s %6s %6s\n", "instrument", "mkt_bid",
                        "mkt_ask", "our_bid", "our_ask", "theo", "mb_iv", "ob_iv", "oa_iv", "ma_iv");
            for (const auto& s : quoted) {
                const auto* q = book.quote(s.id);
                double T, theo_iv;
                if (!q || !theo_for(*q, T, theo_iv)) continue;
                auto iv_of = [&](double px) {
                    if (px <= 0.0) return 0.0;
                    const auto r = od::implied_vol(q->type, od::coin_to_usd(px, q->underlying),
                                                   q->underlying, q->strike, T);
                    return r.ok() ? 100 * r.sigma : 0.0;
                };
                const auto* b = s.bid_order ? om.find(s.bid_order) : nullptr;
                const auto* a = s.ask_order ? om.find(s.ask_order) : nullptr;
                const double ob = b && b->live() ? b->req.price : 0.0;
                const double oa = a && a->live() ? a->req.price : 0.0;
                const double theo = od::usd_to_coin(
                    od::black76_price(q->type, q->underlying, q->strike, T, theo_iv), q->underlying);
                std::printf("  %-22s %8.4f %8.4f | %8.4f %8.4f | %7.4f | %6.2f %6.2f %6.2f %6.2f  theo_iv %.2f\n",
                            s.name.c_str(), q->bid, q->ask, ob, oa, theo, iv_of(q->bid), iv_of(ob),
                            iv_of(oa), iv_of(q->ask), 100 * theo_iv);
            }
        }
        std::fflush(stdout);
    };

    const auto start = std::chrono::steady_clock::now();
    auto next_requote = start, next_risk = start, next_refit = start;
    auto next_status = start + std::chrono::milliseconds(args.status_ms);

    while (!g_stop.load(std::memory_order_relaxed)) {
        od::TickerUpdate u;
        int n = 0;
        for (;;) {
            const auto p = reader->poll(u);
            if (p == od::bus::FeedBusReader::Poll::Empty) break;
            if (p == od::bus::FeedBusReader::Poll::Lapped) {
                for (const auto& s : reader->snapshot()) book.apply(s);
                continue;
            }
            book.apply(u);
            if (quoted_index.contains(u.instrument))
                gw.on_market(table[u.instrument].name, u.bid, u.ask);
            ++n;
        }
        process_events();

        if (g_kill && !gate.killed()) {
            gate.kill();
            gw.cancel_all();
            std::printf("KILL SWITCH: all orders cancelled, new orders blocked\n");
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refit) {
            fitter.post(book.to_chain(ccy));
            next_refit = now + std::chrono::seconds(1);
        }
        view = fitter.latest();
        if (now >= next_risk) {
            refresh_risk();
            if (const double usd = od::hedge_notional_usd(hedgeable_delta(), view->index(), cfg.hedge);
                usd != 0.0) {
                od::OrderRequest h{om.next_id(), ccy + "-PERPETUAL", usd > 0 ? od::Side::Buy : od::Side::Sell,
                                   view->index(), std::abs(usd), false};
                if (const auto why = gate.check(h, {}, od::now_ns())) {
                    ++rejects["hedge:" + *why];
                } else {
                    om.on_sent(h);
                    gw.place(h);
                    ++hedges;
                    process_events();
                    refresh_risk();
                }
            }
            next_risk = now + std::chrono::seconds(1);
        }
        if (now >= next_requote && !gate.killed()) {
            requote();
            process_events();
            next_requote = now + std::chrono::milliseconds(cfg.requote_ms);
        }
        if (now >= next_status) {
            status(false);
            next_status = now + std::chrono::milliseconds(args.status_ms);
        }
        if (args.duration_s > 0 && now - start >= std::chrono::seconds(args.duration_s)) break;
        if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    gw.cancel_all();
    process_events();
    refresh_risk();
    status(true);
    std::printf("\npositions:\n");
    for (const auto& row : risk.rows) {
        if (!row.priced) continue;
        std::printf("  %-22s %+10.2f  delta %+7.3f  vega $%+7.0f\n", row.position->instrument.c_str(),
                    row.position->qty, row.delta, row.vega_usd);
    }
    if (edge_qty > 0)
        std::printf("average model edge at fill: %+.2f vol pts over %.1f contracts\n",
                    edge_sum_vol / edge_qty, edge_qty);
    return 0;
}
