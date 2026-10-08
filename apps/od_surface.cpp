// od_surface: pull a Deribit option chain, rebuild the implied vol surface,
// fit SVI per expiry and print smile metrics + arbitrage diagnostics.
//
//   od_surface --currency BTC                     live snapshot
//   od_surface --currency ETH --watch 30          refresh every 30 s
//   od_surface --save snap.json                   also store the raw snapshot
//   od_surface --snapshot snap.json               offline, reproducible
//   od_surface --csv surface.csv                  per-strike IVs for plotting

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <string>
#include <thread>

#include "od/deribit.hpp"
#include "od/surface.hpp"

namespace {

struct Args {
    std::string currency = "BTC";
    std::string snapshot, save, csv;
    int watch = 0;
    bool testnet = false;
};

void usage() {
    std::puts(
        "usage: od_surface [--currency BTC|ETH] [--snapshot FILE] [--save FILE]\n"
        "                  [--csv FILE] [--watch SECONDS] [--testnet]");
}

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (f == "--testnet") a.testnet = true;
        else if (f == "--help" || f == "-h") return false;
        else if (!(v = val())) return false;
        else if (f == "--currency") a.currency = v;
        else if (f == "--snapshot") a.snapshot = v;
        else if (f == "--save") a.save = v;
        else if (f == "--csv") a.csv = v;
        else if (f == "--watch") a.watch = std::atoi(v);
        else return false;
    }
    return true;
}

std::string utc(std::int64_t ms) {
    const std::time_t t = ms / 1000;
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&t));
    return buf;
}

void write_csv(const std::string& path, const std::vector<od::SmileFit>& fits) {
    std::ofstream out(path);
    out << "expiry,T,forward,strike,k,type,bid_iv,ask_iv,mid_iv,mark_iv,svi_iv,in_fit\n";
    for (const auto& f : fits) {
        for (const auto& s : f.strikes) {
            const auto& q = *s.quote;
            out << f.slice->label << ',' << f.slice->T << ',' << f.slice->forward << ','
                << q.strike << ',' << s.k << ',' << (q.type == od::OptionType::Call ? 'C' : 'P')
                << ',' << s.bid_iv << ',' << s.ask_iv << ',' << s.mid_iv << ',' << q.mark_iv
                << ',' << s.svi_iv << ',' << s.in_fit << '\n';
        }
    }
}

void report(const od::OptionChain& chain, const Args& args) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto fits = od::build_surface(chain);
    const auto t1 = std::chrono::steady_clock::now();
    const auto check = od::check_against_exchange(chain);

    std::size_t n_quotes = 0;
    for (const auto& e : chain.expiries) n_quotes += e.quotes.size();

    std::printf("\n%s options  as of %s  (%zu instruments, %zu expiries)\n",
                chain.currency.c_str(), utc(chain.asof_ms).c_str(), n_quotes,
                chain.expiries.size());
    std::printf("surface built in %.1f ms\n\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count());

    std::printf("%-9s %7s %10s %7s %7s %7s %7s %6s %8s %s\n", "expiry", "days", "forward",
                "inside", "atm_iv", "rr25", "bf25", "rmse", "min_g", "flags");
    for (const auto& f : fits) {
        char inside[16];
        std::snprintf(inside, sizeof inside, "%d/%d", f.n_inside, f.n_fit);
        std::string flags;
        if (f.min_g < 0.0) flags += "BUTTERFLY_ARB ";
        if (f.calendar_violation > 1e-6) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "CALENDAR_ARB(dw=%.5f) ", f.calendar_violation);
            flags += buf;
        }
        std::printf("%-9s %7.2f %10.2f %7s %6.2f%% %+6.2f %+6.2f %6.2f %8.4f %s\n",
                    f.slice->label.c_str(), f.slice->T * 365.0, f.slice->forward, inside,
                    100 * f.atm_iv, 100 * f.rr25, 100 * f.bf25, 100 * f.rmse_vol, f.min_g,
                    flags.c_str());
    }
    std::printf("\ninside = fit quotes where the SVI vol lies within the bid/ask IVs\n"
                "iv, rr25, bf25, rmse in vol points; rmse = SVI vs market mid IV\n");

    std::printf("\npricer cross-check vs exchange marks (%d OTM options, %d skipped for ~zero vega):\n",
                check.n - check.n_low_vega, check.n_low_vega);
    std::printf("  |iv(mark) - mark_iv|  median %.4f  p95 %.4f  max %.4f vol pts  (%d unsolvable)\n",
                100 * check.median_iv_err, 100 * check.p95_iv_err, 100 * check.max_iv_err,
                check.iv_failures);
    std::printf("  (mark_iv is published to 0.01 vol pts, so ~0.005 is a match)\n");

    if (!args.csv.empty()) {
        write_csv(args.csv, fits);
        std::printf("\nwrote %s\n", args.csv.c_str());
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
        do {
            const auto snap = args.snapshot.empty()
                                  ? od::deribit::fetch_snapshot(args.currency, args.testnet)
                                  : od::deribit::load_snapshot(args.snapshot);
            if (!args.save.empty()) od::deribit::save_snapshot(snap, args.save);
            report(od::deribit::to_chain(snap), args);
            if (args.watch > 0) std::this_thread::sleep_for(std::chrono::seconds(args.watch));
        } while (args.watch > 0 && args.snapshot.empty());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
