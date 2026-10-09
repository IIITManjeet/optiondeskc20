// od_risk: Greeks, equity and scenario P&L for a portfolio, valued on the fitted surface.
//
//   od_risk --positions examples/portfolio_btc.json --snapshot data/btc_snapshot.json
//   od_risk --positions my.json --currency BTC              live REST snapshot
//   od_risk --positions my.json --days 7 --sticky-strike     a week of decay, sticky-strike vols
//   od_risk --positions my.json --bus od_feed_BTC            from od_feedd's shared memory

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

#include "od/deribit.hpp"
#include "od/feed_bus.hpp"
#include "od/portfolio_io.hpp"
#include "od/risk.hpp"

namespace {

struct Args {
    std::string positions, snapshot, currency, bus;
    std::vector<double> spot{-0.20, -0.10, -0.05, 0.0, 0.05, 0.10, 0.20};
    std::vector<double> vol{-10, -5, 0, 5, 10};
    double days = 0.0;
    bool sticky_strike = false, testnet = false;
};

std::vector<double> parse_list(const char* s, double scale) {
    std::vector<double> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(std::atof(item.c_str()) * scale);
    return out;
}

void usage() {
    std::puts(
        "usage: od_risk --positions FILE [--snapshot FILE | --currency BTC|ETH | --bus NAME]\n"
        "               [--spot -20,-10,0,10,20] [--vol -10,0,10] [--days N]\n"
        "               [--sticky-strike] [--testnet]\n"
        "  --spot in percent, --vol in vol points");
}

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (f == "--sticky-strike") a.sticky_strike = true;
        else if (f == "--testnet") a.testnet = true;
        else if (f == "--help" || f == "-h") return false;
        else if (!(v = val())) return false;
        else if (f == "--positions") a.positions = v;
        else if (f == "--snapshot") a.snapshot = v;
        else if (f == "--currency") a.currency = v;
        else if (f == "--bus") a.bus = v;
        else if (f == "--spot") a.spot = parse_list(v, 0.01);
        else if (f == "--vol") a.vol = parse_list(v, 1.0);
        else if (f == "--days") a.days = std::atof(v);
        else return false;
    }
    return !a.positions.empty() && !a.spot.empty() && !a.vol.empty();
}

std::string utc(std::int64_t ms, const char* fmt = "%Y-%m-%d %H:%M:%S UTC") {
    const std::time_t t = ms / 1000;
    char buf[40];
    std::strftime(buf, sizeof buf, fmt, std::gmtime(&t));
    return buf;
}

void print_grid(const char* title, const od::ScenarioGrid& g,
                const std::vector<std::vector<double>>& m, const char* fmt) {
    std::printf("\n%s\n%10s", title, "vol \\ spot");
    for (double s : g.spot) std::printf(" %+9.0f%%", 100 * s);
    std::printf("\n");
    for (std::size_t i = 0; i < g.vol_pts.size(); ++i) {
        std::printf("%+7.1f pt", g.vol_pts[i]);
        for (double x : m[i]) std::printf(fmt, x);
        std::printf("\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        usage();
        return 2;
    }
    try {
        const auto pf = od::load_portfolio(args.positions);
        const std::string ccy = args.currency.empty() ? pf.currency : args.currency;
        od::OptionChain chain;
        if (!args.bus.empty()) {
            // Latest state of every instrument straight from the bus's last-value cache.
            auto reader = od::bus::FeedBusReader::open(args.bus);
            const auto table = reader.instruments();
            od::LiveBook book(table);
            for (const auto& u : reader.snapshot()) book.apply(u);
            chain = book.to_chain(reader.currency());
        } else {
            const auto snap = args.snapshot.empty() ? od::deribit::fetch_snapshot(ccy, args.testnet)
                                                    : od::deribit::load_snapshot(args.snapshot);
            chain = od::deribit::to_chain(snap);
        }
        const auto fits = od::build_surface(chain);
        const auto mkt = od::MarketView::build(chain, fits);
        const auto r = od::compute_risk(pf, mkt);

        std::printf("%s portfolio   as of %s   index %.2f\n\n", ccy.c_str(),
                    utc(mkt.asof_ms()).c_str(), mkt.index());
        std::printf("%-22s %9s %9s %9s %9s %7s %4s %8s %8s %8s %9s %9s\n", "instrument", "qty",
                    "mark", "entry", "pnl(c)", "iv", "src", "delta", "delta_pa", "gamma1%",
                    "vega$", "theta$/d");
        for (const auto& row : r.rows) {
            const auto& p = *row.position;
            if (!row.priced) {
                std::printf("%-22s %9.2f   (not in market data)\n", p.instrument.c_str(), p.qty);
                continue;
            }
            if (p.kind == od::PositionKind::Perpetual) {
                std::printf("%-22s %9.0f %9.1f %9.1f %+9.4f %7s %4s %+8.3f %+8.3f\n",
                            p.instrument.c_str(), p.qty, row.mark_coin, p.entry_price,
                            row.pnl_coin, "", "", row.delta, row.delta_premium_adj);
                continue;
            }
            std::printf("%-22s %9.2f %9.5f %9.5f %+9.4f %6.2f%% %4s %+8.3f %+8.3f %+8.3f %+9.1f %+9.1f\n",
                        p.instrument.c_str(), p.qty, row.mark_coin, p.entry_price, row.pnl_coin,
                        100 * row.iv, od::to_string(row.iv_source), row.delta,
                        row.delta_premium_adj, row.gamma_1pct, row.vega_usd, row.theta_usd);
        }
        std::printf("%-22s %9.4f\n", "coin balance", pf.coin_balance);
        if (r.unpriced) std::printf("\nwarning: %d position(s) not priced\n", r.unpriced);

        std::printf("\nequity           : %.4f %s  ($%.0f)\n", r.equity_coin, ccy.c_str(), r.equity_usd);
        std::printf("delta, USD view  : %+.4f %s   (USD P&L for +1%% spot: $%+.0f)\n",
                    r.delta_usd_view, ccy.c_str(), r.delta_usd_view * mkt.index() * 0.01);
        std::printf("delta, coin view : %+.4f %s   (%s P&L for +1%% spot: %+.5f)\n",
                    r.delta_coin_view, ccy.c_str(), ccy.c_str(), r.delta_coin_view * 0.01);
        std::printf("gamma, USD view  : %+.4f %s of delta per 1%% move\n", r.gamma_usd_view, ccy.c_str());
        std::printf("vega             : $%+.0f per vol point\n", r.vega_usd);
        std::printf("theta            : $%+.0f per day\n", r.theta_usd);
        std::printf("\nvega by expiry ($ per vol pt):");
        for (const auto& [exp, v] : r.vega_by_expiry) std::printf("  %s %+.0f", utc(exp, "%d%b%y").c_str(), v);
        std::printf("\n");

        const auto g = od::scenario_grid(pf, mkt, args.spot, args.vol, args.days, args.sticky_strike);
        char title[160];
        std::snprintf(title, sizeof title, "scenario P&L in USD  (%s, %.0f days forward)",
                      args.sticky_strike ? "sticky-strike" : "sticky-moneyness", args.days);
        print_grid(title, g, g.pnl_usd, " %+10.0f");
        std::snprintf(title, sizeof title, "scenario P&L in %s", ccy.c_str());
        print_grid(title, g, g.pnl_coin, " %+10.4f");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
