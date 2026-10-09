// od_replay: run the market maker over a recorded journal, as fast as possible.
//
//   od_replay --journal data/btc.odj --config examples/mm_btc.json
//   od_replay --journal data/btc.odj --config a.json --config b.json   compare configs
//
// The engine sees exactly what it would have seen live: the same events in the
// same order, and a clock that is the recorded receive time of each event.
// Surface refits run synchronously at the moment they are requested, so a replay
// is deterministic: same journal + same config = same fills, every time.
//
// What a replay can't reproduce: our orders never existed in the real book, so the
// market didn't react to them. Fills come from the queue model (see SimGateway).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "od/journal.hpp"
#include "od/mm_engine.hpp"
#include "od/surface.hpp"

namespace {

struct Result {
    std::string config;
    std::uint64_t events = 0;
    double wall_s = 0, sim_s = 0;
    od::MmStats stats;
    std::uint64_t fills = 0;
    double volume = 0, pnl = 0, fees = 0;
};

Result run(const std::string& journal_path, const std::string& config_path, bool verbose,
           int status_ms) {
    od::journal::Reader j(journal_path);
    const auto cfg = od::load_mm_config(config_path);
    od::SimGateway gw;
    std::FILE* out = verbose ? stdout : std::fopen("/dev/null", "w");

    std::unique_ptr<od::MarketMaker> mm;
    mm = std::make_unique<od::MarketMaker>(
        cfg, j.instruments(), j.currency(), gw, &gw,
        [&mm](od::OptionChain chain) {
            const auto fits = od::build_surface(chain);
            mm->set_view(std::make_shared<const od::MarketView>(od::MarketView::build(chain, fits)));
        },
        out, status_ms);

    Result r;
    r.config = config_path;
    const auto wall0 = std::chrono::steady_clock::now();
    od::journal::Event e;
    std::int64_t first_ts = 0, last_ts = 0;
    while (j.next(e)) {
        if (first_ts == 0) first_ts = e.ts_ns;
        last_ts = e.ts_ns;
        if (e.type == od::journal::RecordType::Ticker) mm->on_ticker(e.ticker);
        else mm->on_trade(e.trade);
        mm->on_timer(od::Clock::time_point(std::chrono::nanoseconds(e.ts_ns)));
        ++r.events;
    }
    mm->finish();
    r.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    r.sim_s = (last_ts - first_ts) / 1e9;
    r.stats = mm->stats();
    r.fills = mm->orders().fills();
    r.volume = mm->orders().volume();
    r.pnl = mm->pnl_coin();
    r.fees = mm->orders().fees_paid();
    if (!verbose) std::fclose(out);
    if (j.truncated()) std::fprintf(stderr, "note: journal ends in a truncated record (ignored)\n");
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::string journal;
    std::vector<std::string> configs;
    int status_ms = 60000;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        if (f == "--quiet") quiet = true;
        else if (i + 1 >= argc) break;
        else if (f == "--journal") journal = argv[++i];
        else if (f == "--config") configs.push_back(argv[++i]);
        else if (f == "--status-ms") status_ms = std::atoi(argv[++i]);
    }
    if (journal.empty() || configs.empty()) {
        std::puts("usage: od_replay --journal FILE --config FILE [--config FILE ...] [--status-ms N] [--quiet]");
        return 2;
    }
    try {
        std::vector<Result> results;
        for (const auto& c : configs) {
            if (!quiet) std::printf("=== %s\n", c.c_str());
            results.push_back(run(journal, c, !quiet, status_ms));
        }
        std::printf("\n%-34s %9s %7s %7s %7s %10s %9s %13s %13s\n", "config", "events", "x real",
                    "fills", "volume", "pnl(coin)", "fees", "edge@fill", "markout 5s");
        for (const auto& r : results) {
            const auto& s = r.stats;
            const double edge = s.edge_qty > 0 ? s.edge_vol_qty / s.edge_qty : 0.0;
            const double m5 = s.mark_qty[1] > 0 ? s.mark_vol_qty[1] / s.mark_qty[1] : 0.0;
            std::printf("%-34s %9llu %7.0f %7llu %7.1f %+10.5f %9.5f %+9.2f vol %+9.2f vol\n",
                        r.config.c_str(), static_cast<unsigned long long>(r.events),
                        r.wall_s > 0 ? r.sim_s / r.wall_s : 0.0, static_cast<unsigned long long>(r.fills),
                        r.volume, r.pnl, r.fees, edge, m5);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
