// od_live: streaming vol surface.
//
//   feed thread    : Deribit WebSocket -> parse -> TickerUpdate -> SPSC ring
//   main (pricer)  : ring -> LiveBook; every --refresh-ms, snapshot the book
//   surface thread : fit SVI on the latest snapshot -> dashboard
//
// The pricer never fits on its own thread: a 20-30 ms refit there would stall the
// ring drain and show up as a latency spike on every update queued behind it.
//
//   od_live --currency BTC
//   od_live --currency ETH --feed-cpu 2 --pricer-cpu 3 --busy-poll
//   od_live --duration 60 --plain          run for a minute, log lines, print latency summary

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "od/deribit.hpp"
#include "od/deribit_feed.hpp"
#include "od/histogram.hpp"
#include "od/surface.hpp"
#include "od/thread_util.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

struct Args {
    std::string currency = "BTC";
    int refresh_ms = 1000;
    int feed_cpu = -1, pricer_cpu = -1;
    int duration_s = 0;
    int ring_pow2 = 16;
    bool busy_poll = false, plain = false, testnet = false;
};

void usage() {
    std::puts(
        "usage: od_live [--currency BTC|ETH] [--refresh-ms N] [--duration S]\n"
        "               [--feed-cpu N] [--pricer-cpu N] [--busy-poll] [--ring-pow2 N]\n"
        "               [--plain] [--testnet]");
}

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (f == "--busy-poll") a.busy_poll = true;
        else if (f == "--plain") a.plain = true;
        else if (f == "--testnet") a.testnet = true;
        else if (f == "--help" || f == "-h") return false;
        else if (!(v = val())) return false;
        else if (f == "--currency") a.currency = v;
        else if (f == "--refresh-ms") a.refresh_ms = std::atoi(v);
        else if (f == "--duration") a.duration_s = std::atoi(v);
        else if (f == "--feed-cpu") a.feed_cpu = std::atoi(v);
        else if (f == "--pricer-cpu") a.pricer_cpu = std::atoi(v);
        else if (f == "--ring-pow2") a.ring_pow2 = std::atoi(v);
        else return false;
    }
    return a.refresh_ms > 0 && a.ring_pow2 >= 4 && a.ring_pow2 <= 24;
}

std::int64_t system_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string utc(std::int64_t ms) {
    const std::time_t t = ms / 1000;
    char buf[32];
    std::strftime(buf, sizeof buf, "%H:%M:%S UTC", std::gmtime(&t));
    return buf;
}

double us(std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; }

void print_latency(const char* name, const od::LatencyHistogram& h) {
    std::printf("  %-22s p50 %8.1f  p90 %8.1f  p99 %8.1f  max %9.1f us  (n=%llu)\n", name,
                us(h.percentile(0.5)), us(h.percentile(0.9)), us(h.percentile(0.99)), us(h.max()),
                static_cast<unsigned long long>(h.count()));
}

struct Window {
    od::LatencyHistogram parse, hop;
    std::uint64_t updates = 0;
    std::size_t max_depth = 0;
    void reset() { *this = Window{}; }
};

// Everything the surface thread needs, copied out of the pricer's state.
struct Job {
    od::OptionChain chain;
    Window window;
    double window_s = 0;
    std::size_t live = 0;
    double snapshot_us = 0;
};

void render(const Args& args, const Job& job, const std::vector<od::SmileFit>& fits, double fit_ms,
            const od::deribit::FeedCounters& c, std::size_t n_instruments) {
    const Window& w = job.window;
    if (args.plain) {
        const od::SmileFit* front = fits.empty() ? nullptr : &fits.front();
        std::printf("%s live=%zu/%zu upd/s=%.0f parse_p50=%.1fus hop_p50=%.1fus hop_p99=%.1fus "
                    "snap=%.0fus fit=%.1fms drops=%llu front=%s atm=%.2f%%\n",
                    utc(job.chain.asof_ms).c_str(), job.live, n_instruments,
                    w.updates / job.window_s, us(w.parse.percentile(0.5)),
                    us(w.hop.percentile(0.5)), us(w.hop.percentile(0.99)), job.snapshot_us, fit_ms,
                    static_cast<unsigned long long>(c.ring_full.load()),
                    front ? front->slice->label.c_str() : "-", front ? 100 * front->atm_iv : 0.0);
        std::fflush(stdout);
        return;
    }

    std::printf("\033[H\033[J");  // cursor home, clear screen
    std::printf("%s options  live  exch time %s   %s  subscribed %llu/%zu  live %zu\n",
                args.currency.c_str(), utc(job.chain.asof_ms).c_str(),
                c.connected.load() ? "CONNECTED" : "DISCONNECTED",
                static_cast<unsigned long long>(c.subscribed.load()), n_instruments, job.live);
    std::printf("\n%-9s %7s %10s %7s %8s %7s %7s %6s %s\n", "expiry", "days", "forward", "inside",
                "atm_iv", "rr25", "bf25", "rmse", "flags");
    for (const auto& f : fits) {
        char inside[16];
        std::snprintf(inside, sizeof inside, "%d/%d", f.n_inside, f.n_fit);
        std::string flags;
        if (f.min_g < 0.0) flags += "BUTTERFLY_ARB ";
        if (f.calendar_violation > 1e-6) flags += "CALENDAR_ARB ";
        std::printf("%-9s %7.2f %10.2f %7s %7.2f%% %+6.2f %+6.2f %6.2f %s\n",
                    f.slice->label.c_str(), f.slice->T * 365.0, f.slice->forward, inside,
                    100 * f.atm_iv, 100 * f.rr25, 100 * f.bf25, 100 * f.rmse_vol, flags.c_str());
    }
    std::printf("\nfeed (last %.1fs): %.0f updates/s, ring max depth %zu\n", job.window_s,
                w.updates / job.window_s, w.max_depth);
    std::printf("surface: snapshot %.0f us on pricer thread, fit %.1f ms on surface thread\n",
                job.snapshot_us, fit_ms);
    print_latency("parse (recv->parsed)", w.parse);
    print_latency("hop (parsed->pricer)", w.hop);
    std::printf("  totals: frames %llu  tickers %llu  drops %llu  unknown %llu  heartbeats %llu  reconnects %llu\n",
                static_cast<unsigned long long>(c.frames.load()),
                static_cast<unsigned long long>(c.tickers.load()),
                static_cast<unsigned long long>(c.ring_full.load()),
                static_cast<unsigned long long>(c.unknown.load()),
                static_cast<unsigned long long>(c.heartbeats.load()),
                static_cast<unsigned long long>(c.reconnects.load()));
    std::printf("\nCtrl-C to quit\n");
    std::fflush(stdout);
}

// Latest-wins mailbox + worker. If a fit is still running when the next snapshot
// arrives, the older pending snapshot is replaced: the dashboard only wants the
// newest surface, and the pricer must never wait on the fitter.
class SurfaceWorker {
public:
    SurfaceWorker(const Args& args, const od::deribit::FeedCounters& c, std::size_t n)
        : args_(args), counters_(c), n_instruments_(n), thread_([this] { loop(); }) {}

    ~SurfaceWorker() { stop(); }

    void post(Job job) {
        {
            std::lock_guard lk(m_);  // held only for a move
            if (pending_) ++superseded_;
            pending_ = std::move(job);
        }
        cv_.notify_one();
    }

    // Joins the worker; a fit in flight completes first. Stats are safe to read after.
    void stop() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    const od::LatencyHistogram& fit_hist() const { return fit_hist_; }
    std::uint64_t superseded() const { return superseded_; }

private:
    void loop() {
        od::name_current_thread("od-surface");
        for (;;) {
            Job job;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return stop_ || pending_; });
                if (stop_) return;
                job = std::move(*pending_);
                pending_.reset();
            }
            const auto t0 = od::now_ns();
            const auto fits = od::build_surface(job.chain);
            const auto fit_ns = od::now_ns() - t0;
            fit_hist_.record(static_cast<std::uint64_t>(fit_ns));
            render(args_, job, fits, fit_ns / 1e6, counters_, n_instruments_);
        }
    }

    const Args& args_;
    const od::deribit::FeedCounters& counters_;
    const std::size_t n_instruments_;
    std::mutex m_;
    std::condition_variable cv_;
    std::optional<Job> pending_;
    bool stop_ = false;
    std::uint64_t superseded_ = 0;
    od::LatencyHistogram fit_hist_;
    std::thread thread_;  // last: starts only after every member above is constructed
};

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

    od::InstrumentTable table;
    try {
        table = od::deribit::fetch_instrument_table(args.currency, args.testnet);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "loaded %zu %s options\n", table.size(), args.currency.c_str());

    od::SpscRing<od::TickerUpdate> ring(std::size_t{1} << args.ring_pow2);
    od::deribit::FeedCounters counters;
    od::deribit::Feed feed({.testnet = args.testnet}, table, ring, counters);

    std::thread feed_thread([&] {
        od::name_current_thread("od-feed");
        if (args.feed_cpu >= 0 && !od::pin_current_thread(args.feed_cpu))
            std::fprintf(stderr, "warning: could not pin feed thread to cpu %d\n", args.feed_cpu);
        feed.run(g_stop);
    });

    SurfaceWorker surface(args, counters, table.size());

    od::name_current_thread("od-pricer");
    if (args.pricer_cpu >= 0 && !od::pin_current_thread(args.pricer_cpu))
        std::fprintf(stderr, "warning: could not pin pricer thread to cpu %d\n", args.pricer_cpu);

    od::LiveBook book(table);
    od::LatencyHistogram total_parse, total_hop, total_snapshot, exch_age;
    Window window;
    const auto start = std::chrono::steady_clock::now();
    auto window_start = start;
    auto next_refresh = start + std::chrono::milliseconds(args.refresh_ms);

    while (!g_stop.load(std::memory_order_relaxed)) {
        window.max_depth = std::max(window.max_depth, ring.size());
        od::TickerUpdate u;
        int drained = 0;
        while (ring.try_pop(u)) {
            const std::int64_t t = od::now_ns();
            window.hop.record(static_cast<std::uint64_t>(t - u.parsed_ns));
            window.parse.record(static_cast<std::uint64_t>(u.parsed_ns - u.recv_ns));
            const std::int64_t age_ms = system_ms() - u.exch_ts_ms;
            if (age_ms >= 0) exch_age.record(static_cast<std::uint64_t>(age_ms) * 1'000'000);
            book.apply(u);
            ++window.updates;
            ++drained;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refresh) {
            const auto t0 = od::now_ns();
            Job job{book.to_chain(args.currency), window,
                    std::chrono::duration<double>(now - window_start).count(), book.live_count(), 0};
            const auto snap_ns = od::now_ns() - t0;
            job.snapshot_us = snap_ns / 1e3;
            total_snapshot.record(static_cast<std::uint64_t>(snap_ns));
            surface.post(std::move(job));

            total_parse.merge(window.parse);
            total_hop.merge(window.hop);
            window.reset();
            window_start = now;
            next_refresh = now + std::chrono::milliseconds(args.refresh_ms);
        }

        if (args.duration_s > 0 && now - start >= std::chrono::seconds(args.duration_s)) break;

        // Busy-polling keeps the hop latency at cache-transfer speed but burns a core.
        // Sleeping frees the core, at the cost of the scheduler's wake-up latency.
        if (drained == 0 && !args.busy_poll) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    g_stop = true;
    feed.interrupt();
    feed_thread.join();
    surface.stop();
    total_parse.merge(window.parse);
    total_hop.merge(window.hop);

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("\nrun summary: %.1fs, %llu tickers (%.0f/s), drops %llu, reconnects %llu, %s\n",
                secs, static_cast<unsigned long long>(counters.tickers.load()),
                counters.tickers.load() / secs,
                static_cast<unsigned long long>(counters.ring_full.load()),
                static_cast<unsigned long long>(counters.reconnects.load()),
                args.busy_poll ? "busy-poll" : "sleep-poll");
    print_latency("parse (recv->parsed)", total_parse);
    print_latency("hop (parsed->pricer)", total_hop);
    print_latency("book snapshot", total_snapshot);
    print_latency("surface fit", surface.fit_hist());
    std::printf("  snapshots superseded before fitting: %llu\n",
                static_cast<unsigned long long>(surface.superseded()));
    std::printf("  exch->pricer p50 %.0f ms (includes local clock offset vs exchange)\n",
                us(exch_age.percentile(0.5)) / 1000.0);
    return 0;
}
