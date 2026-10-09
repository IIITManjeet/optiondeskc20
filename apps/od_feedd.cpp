// od_feedd: market-data daemon. Owns the Deribit connection and publishes every
// ticker into a shared-memory bus that any number of local processes can read.
//
//   od_feedd --currency BTC                    -> /dev/shm/od_feed_BTC
//   od_feedd --currency ETH --feed-cpu 2
//   od_live --bus od_feed_BTC                  (in another terminal; run several)
//
// The feed thread parses and publishes straight into the bus (no intermediate
// queue). The main thread only writes the heartbeat and mirrors feed statistics
// into the segment for readers to display.

#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include "od/deribit.hpp"
#include "od/deribit_feed.hpp"
#include "od/feed_bus.hpp"
#include "od/thread_util.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

struct Args {
    std::string currency = "BTC";
    std::string name;  // default od_feed_<CCY>
    int capacity_pow2 = 16;
    int feed_cpu = -1;
    int stats_s = 10;
    bool testnet = false;
};

void usage() {
    std::puts(
        "usage: od_feedd [--currency BTC|ETH] [--name SHM_NAME] [--capacity-pow2 N]\n"
        "                [--feed-cpu N] [--stats-s N] [--testnet]");
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
        else if (f == "--name") a.name = v;
        else if (f == "--capacity-pow2") a.capacity_pow2 = std::atoi(v);
        else if (f == "--feed-cpu") a.feed_cpu = std::atoi(v);
        else if (f == "--stats-s") a.stats_s = std::atoi(v);
        else return false;
    }
    if (a.name.empty()) a.name = "od_feed_" + a.currency;
    return a.capacity_pow2 >= 4 && a.capacity_pow2 <= 24 && a.stats_s > 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        usage();
        return 2;
    }
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    try {
        const auto table = od::deribit::fetch_instrument_table(args.currency, args.testnet);
        auto bus = od::bus::FeedBusWriter::create(args.name, args.currency, table,
                                                  std::size_t{1} << args.capacity_pow2);
        std::fprintf(stderr, "od_feedd: %zu %s options -> /dev/shm/%s (%zu slots)\n", table.size(),
                     args.currency.c_str(), args.name.c_str(), std::size_t{1} << args.capacity_pow2);

        od::deribit::FeedCounters counters;
        od::deribit::Feed feed(
            {.testnet = args.testnet}, table,
            [&bus](const od::TickerUpdate& u) {
                bus.publish(u);  // never refuses: slow readers get lapped instead
                return true;
            },
            counters);

        std::thread feed_thread([&] {
            od::name_current_thread("od-feed");
            if (args.feed_cpu >= 0 && !od::pin_current_thread(args.feed_cpu))
                std::fprintf(stderr, "warning: could not pin feed thread to cpu %d\n", args.feed_cpu);
            feed.run(g_stop);
        });

        auto next_stats = std::chrono::steady_clock::now() + std::chrono::seconds(args.stats_s);
        std::uint64_t last_tickers = 0;
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            bus.heartbeat(od::now_ns());
            const auto stats = od::deribit::snapshot(counters);
            bus.set_stats(stats);
            if (const auto now = std::chrono::steady_clock::now(); now >= next_stats) {
                std::fprintf(stderr, "od_feedd: %s, %.0f tickers/s, total %llu, reconnects %llu\n",
                             stats.connected ? "connected" : "DISCONNECTED",
                             static_cast<double>(stats.tickers - last_tickers) / args.stats_s,
                             static_cast<unsigned long long>(stats.tickers),
                             static_cast<unsigned long long>(stats.reconnects));
                last_tickers = stats.tickers;
                next_stats = now + std::chrono::seconds(args.stats_s);
            }
        }
        feed.interrupt();
        feed_thread.join();
        std::fprintf(stderr, "od_feedd: stopped after %llu updates\n",
                     static_cast<unsigned long long>(bus.published()));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
