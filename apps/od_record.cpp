// od_record: record the feed buses to a journal for later replay.
//
//   od_feedd --currency BTC                               (terminal 1)
//   od_record --out data/btc_2026-10-10.odj --duration 3600
//
// Starts with the full current state (last-value cache) so a replay has every
// instrument from its first event, then appends every ticker and trade in
// arrival order. If it ever falls a ring behind on tickers it re-snapshots the
// state (and says so); missed trades can't be recovered and are counted.

#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#include "od/journal.hpp"
#include "od/thread_util.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }
}  // namespace

int main(int argc, char** argv) {
    std::string bus = "od_feed_BTC", trades_bus, out;
    int duration_s = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string f = argv[i];
        if (f == "--bus") bus = argv[i + 1];
        else if (f == "--trades-bus") trades_bus = argv[i + 1];
        else if (f == "--out") out = argv[i + 1];
        else if (f == "--duration") duration_s = std::atoi(argv[i + 1]);
    }
    if (out.empty()) {
        std::puts("usage: od_record --out FILE [--bus SHM_NAME] [--trades-bus SHM_NAME] [--duration S]");
        return 2;
    }
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    try {
        auto tickers = od::bus::FeedBusReader::open(bus);
        const auto ccy = tickers.currency();
        std::optional<od::bus::TradeBusReader> trades;
        try {
            trades.emplace(od::bus::TradeBusReader::open(trades_bus.empty() ? "od_trades_" + ccy : trades_bus));
            trades->seek_to_end();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "warning: recording tickers only (%s)\n", e.what());
        }

        od::journal::Writer w(out, ccy, tickers.instruments());
        const std::int64_t now = od::now_ns();
        for (auto u : tickers.snapshot()) {
            u.recv_ns = now;  // state as of the start of the recording
            w.write(u);
        }

        std::uint64_t resyncs = 0, lost_trade_laps = 0;
        const auto start = std::chrono::steady_clock::now();
        auto next_log = start + std::chrono::seconds(10);
        while (!g_stop.load()) {
            int n = 0;
            od::TickerUpdate u;
            for (od::bus::FeedBusReader::Poll p; (p = tickers.poll(u)) != od::bus::FeedBusReader::Poll::Empty;) {
                if (p == od::bus::FeedBusReader::Poll::Lapped) {
                    ++resyncs;
                    const std::int64_t t = od::now_ns();
                    for (auto s : tickers.snapshot()) {
                        s.recv_ns = t;
                        w.write(s);
                    }
                    continue;
                }
                w.write(u);
                ++n;
            }
            if (trades) {
                od::TradeUpdate t;
                for (od::bus::TradeBusReader::Poll p; (p = trades->poll(t)) != od::bus::TradeBusReader::Poll::Empty;) {
                    if (p == od::bus::TradeBusReader::Poll::Lapped) {
                        ++lost_trade_laps;
                        trades->seek_to_end();
                        continue;
                    }
                    w.write(t);
                    ++n;
                }
            }
            const auto now_tp = std::chrono::steady_clock::now();
            if (now_tp >= next_log) {
                w.flush();
                std::fprintf(stderr, "od_record: %llu records, %.1f MB\n",
                             static_cast<unsigned long long>(w.records()), w.bytes() / 1e6);
                next_log = now_tp + std::chrono::seconds(10);
            }
            if (duration_s > 0 && now_tp - start >= std::chrono::seconds(duration_s)) break;
            if (n == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::fprintf(stderr, "od_record: wrote %llu records (%.1f MB) to %s; resyncs %llu, trade laps %llu\n",
                     static_cast<unsigned long long>(w.records()), w.bytes() / 1e6, out.c_str(),
                     static_cast<unsigned long long>(resyncs),
                     static_cast<unsigned long long>(lost_trade_laps));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
