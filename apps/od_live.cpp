// od_live: streaming vol surface.
//
// Standalone (own feed):
//   feed thread    : Deribit WebSocket -> parse -> TickerUpdate -> SPSC ring
//   main (pricer)  : ring -> LiveBook; every --refresh-ms, snapshot the book
//   surface thread : fit SVI on the latest snapshot -> dashboard
//
// Bus mode (--bus NAME): no feed of its own. The pricer reads the shared-memory
// bus published by od_feedd, so several od_live / od_risk processes can share
// one exchange connection.
//
// The pricer never fits on its own thread: a 20-30 ms refit there would stall the
// ring drain and show up as a latency spike on every update queued behind it.
//
//   od_live --currency BTC
//   od_live --currency ETH --feed-cpu 2 --pricer-cpu 3 --busy-poll
//   od_live --bus od_feed_BTC --pricer-cpu 4 --busy-poll
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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "od/deribit.hpp"
#include "od/deribit_feed.hpp"
#include "od/feed_bus.hpp"
#include "od/histogram.hpp"
#include "od/spsc_ring.hpp"
#include "od/surface.hpp"
#include "od/thread_util.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

struct Args {
    std::string currency = "BTC";
    std::string bus;
    int refresh_ms = 1000;
    int feed_cpu = -1, pricer_cpu = -1;
    int duration_s = 0;
    int ring_pow2 = 16;
    bool busy_poll = false, plain = false, testnet = false;
};

void usage() {
    std::puts(
        "usage: od_live [--currency BTC|ETH | --bus SHM_NAME] [--refresh-ms N] [--duration S]\n"
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
        else if (f == "--bus") a.bus = v;
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
    std::size_t live = 0, n_instruments = 0;
    double snapshot_us = 0;
    od::deribit::FeedStats stats;
    std::uint64_t laps = 0;
    bool producer_stale = false;
};

void render(const Args& args, const Job& job, const std::vector<od::SmileFit>& fits, double fit_ms) {
    const Window& w = job.window;
    const auto& c = job.stats;
    const char* hop_name = args.bus.empty() ? "hop (parsed->pricer)" : "hop (feedd->this proc)";
    if (args.plain) {
        const od::SmileFit* front = fits.empty() ? nullptr : &fits.front();
        std::printf("%s live=%zu/%zu upd/s=%.0f parse_p50=%.1fus hop_p50=%.1fus hop_p99=%.1fus "
                    "snap=%.0fus fit=%.1fms drops=%llu laps=%llu%s front=%s atm=%.2f%%\n",
                    utc(job.chain.asof_ms).c_str(), job.live, job.n_instruments,
                    w.updates / job.window_s, us(w.parse.percentile(0.5)),
                    us(w.hop.percentile(0.5)), us(w.hop.percentile(0.99)), job.snapshot_us, fit_ms,
                    static_cast<unsigned long long>(c.ring_full),
                    static_cast<unsigned long long>(job.laps),
                    job.producer_stale ? " PRODUCER_STALE" : "",
                    front ? front->slice->label.c_str() : "-", front ? 100 * front->atm_iv : 0.0);
        std::fflush(stdout);
        return;
    }

    std::printf("\033[H\033[J");  // cursor home, clear screen
    std::printf("%s options  live  exch time %s   %s%s  subscribed %llu/%zu  live %zu%s\n",
                job.chain.currency.c_str(), utc(job.chain.asof_ms).c_str(),
                c.connected ? "CONNECTED" : "DISCONNECTED",
                args.bus.empty() ? "" : ("  via bus " + args.bus).c_str(),
                static_cast<unsigned long long>(c.subscribed), job.n_instruments, job.live,
                job.producer_stale ? "  PRODUCER STALE" : "");
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
    std::printf("\nfeed (last %.1fs): %.0f updates/s, %s\n", job.window_s, w.updates / job.window_s,
                args.bus.empty() ? ("ring max depth " + std::to_string(w.max_depth)).c_str()
                                 : ("bus laps " + std::to_string(job.laps)).c_str());
    std::printf("surface: snapshot %.0f us on pricer thread, fit %.1f ms on surface thread\n",
                job.snapshot_us, fit_ms);
    print_latency("parse (recv->parsed)", w.parse);
    print_latency(hop_name, w.hop);
    std::printf("  totals: frames %llu  tickers %llu  drops %llu  unknown %llu  heartbeats %llu  reconnects %llu\n",
                static_cast<unsigned long long>(c.frames), static_cast<unsigned long long>(c.tickers),
                static_cast<unsigned long long>(c.ring_full), static_cast<unsigned long long>(c.unknown),
                static_cast<unsigned long long>(c.heartbeats),
                static_cast<unsigned long long>(c.reconnects));
    std::printf("\nCtrl-C to quit\n");
    std::fflush(stdout);
}

// Latest-wins mailbox + worker. If a fit is still running when the next snapshot
// arrives, the older pending snapshot is replaced: the dashboard only wants the
// newest surface, and the pricer must never wait on the fitter.
class SurfaceWorker {
public:
    explicit SurfaceWorker(const Args& args) : args_(args), thread_([this] { loop(); }) {}

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
            render(args_, job, fits, fit_ns / 1e6);
        }
    }

    const Args& args_;
    std::mutex m_;
    std::condition_variable cv_;
    std::optional<Job> pending_;
    bool stop_ = false;
    std::uint64_t superseded_ = 0;
    od::LatencyHistogram fit_hist_;
    std::thread thread_;  // last: starts only after every member above is constructed
};

// Market-data source for the pricer: either our own feed thread + SPSC ring, or
// the shared-memory bus. Both deliver TickerUpdates; only the plumbing differs.
class Source {
public:
    virtual ~Source() = default;
    virtual const od::InstrumentTable& table() const = 0;
    virtual std::string currency() const = 0;
    // Pops one update if available. May also call `reset` with a full state snapshot
    // (bus attach/lap/reattach); the pricer then rebuilds its book from it.
    virtual bool poll(od::TickerUpdate& u) = 0;
    virtual od::deribit::FeedStats stats() const = 0;
    virtual std::size_t depth() const { return 0; }
    virtual std::uint64_t laps() const { return 0; }
    virtual bool stale() const { return false; }
    virtual void stop() {}

    std::function<void(const std::vector<od::TickerUpdate>&, bool table_changed)> reset;
};

class RingSource : public Source {
public:
    RingSource(const Args& args)
        : currency_(args.currency),
          table_(od::deribit::fetch_instrument_table(args.currency, args.testnet)),
          ring_(std::size_t{1} << args.ring_pow2),
          feed_({.testnet = args.testnet}, table_,
                [this](const od::TickerUpdate& u) { return ring_.try_push(u); }, counters_),
          thread_([this, cpu = args.feed_cpu] {
              od::name_current_thread("od-feed");
              if (cpu >= 0 && !od::pin_current_thread(cpu))
                  std::fprintf(stderr, "warning: could not pin feed thread to cpu %d\n", cpu);
              feed_.run(g_stop);
          }) {}
    ~RingSource() override { stop(); }

    const od::InstrumentTable& table() const override { return table_; }
    std::string currency() const override { return currency_; }
    bool poll(od::TickerUpdate& u) override { return ring_.try_pop(u); }
    od::deribit::FeedStats stats() const override { return od::deribit::snapshot(counters_); }
    std::size_t depth() const override { return ring_.size(); }
    void stop() override {
        if (thread_.joinable()) {
            feed_.interrupt();
            thread_.join();
        }
    }

private:
    std::string currency_;
    od::InstrumentTable table_;
    od::SpscRing<od::TickerUpdate> ring_;
    od::deribit::FeedCounters counters_;
    od::deribit::Feed feed_;
    std::thread thread_;  // last
};

class BusSource : public Source {
public:
    explicit BusSource(std::string name)
        : name_(std::move(name)), reader_(od::bus::FeedBusReader::open(name_)) {
        table_ = reader_->instruments();
    }

    const od::InstrumentTable& table() const override { return table_; }
    std::string currency() const override { return reader_->currency(); }
    od::deribit::FeedStats stats() const override { return reader_->stats(); }
    std::uint64_t laps() const override { return laps_ + reader_->laps(); }
    bool stale() const override { return stale_; }

    // Called once the pricer has installed `reset`.
    void start() { reset(reader_->snapshot(), false); }

    bool poll(od::TickerUpdate& u) override {
        switch (reader_->poll(u)) {
            case od::bus::FeedBusReader::Poll::Update:
                return true;
            case od::bus::FeedBusReader::Poll::Lapped:
                reset(reader_->snapshot(), false);  // skipped updates: take full state
                return false;
            case od::bus::FeedBusReader::Poll::Empty:
                check_producer();
                return false;
        }
        return false;
    }

private:
    // A heartbeat older than 2 s means the producer died or restarted. Try to attach
    // to a new segment of the same name; if its epoch differs, start over from it.
    void check_producer() {
        const auto now = od::now_ns();
        if (now < next_check_) return;
        next_check_ = now + 200'000'000;
        stale_ = now - reader_->heartbeat_ns() > 2'000'000'000;
        if (!stale_) return;
        try {
            auto fresh = od::bus::FeedBusReader::open(name_);
            if (fresh.epoch() == reader_->epoch()) return;
            laps_ += reader_->laps();
            reader_ = std::move(fresh);
            table_ = reader_->instruments();
            stale_ = false;
            std::fprintf(stderr, "od_live: reattached to restarted producer on %s\n", name_.c_str());
            reset(reader_->snapshot(), true);
        } catch (const std::exception&) {
            // Producer not back yet; keep showing the last state, flagged stale.
        }
    }

    std::string name_;
    std::optional<od::bus::FeedBusReader> reader_;
    od::InstrumentTable table_;
    std::uint64_t laps_ = 0;
    std::int64_t next_check_ = 0;
    bool stale_ = false;
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

    std::unique_ptr<Source> source;
    BusSource* bus_source = nullptr;
    try {
        if (args.bus.empty()) {
            source = std::make_unique<RingSource>(args);
        } else {
            auto b = std::make_unique<BusSource>(args.bus);
            bus_source = b.get();
            source = std::move(b);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "%s: %zu %s options\n", args.bus.empty() ? "feed" : "bus",
                 source->table().size(), source->currency().c_str());

    SurfaceWorker surface(args);

    od::name_current_thread("od-pricer");
    if (args.pricer_cpu >= 0 && !od::pin_current_thread(args.pricer_cpu))
        std::fprintf(stderr, "warning: could not pin pricer thread to cpu %d\n", args.pricer_cpu);

    auto book = std::make_unique<od::LiveBook>(source->table());
    source->reset = [&](const std::vector<od::TickerUpdate>& state, bool table_changed) {
        if (table_changed) book = std::make_unique<od::LiveBook>(source->table());
        for (const auto& u : state) book->apply(u);
    };
    if (bus_source) bus_source->start();

    od::LatencyHistogram total_parse, total_hop, total_snapshot, exch_age;
    Window window;
    const auto start = std::chrono::steady_clock::now();
    auto window_start = start;
    auto next_refresh = start + std::chrono::milliseconds(args.refresh_ms);

    while (!g_stop.load(std::memory_order_relaxed)) {
        window.max_depth = std::max(window.max_depth, source->depth());
        od::TickerUpdate u;
        int drained = 0;
        while (source->poll(u)) {
            const std::int64_t t = od::now_ns();
            window.hop.record(static_cast<std::uint64_t>(t - u.parsed_ns));
            window.parse.record(static_cast<std::uint64_t>(u.parsed_ns - u.recv_ns));
            const std::int64_t age_ms = system_ms() - u.exch_ts_ms;
            if (age_ms >= 0) exch_age.record(static_cast<std::uint64_t>(age_ms) * 1'000'000);
            book->apply(u);
            ++window.updates;
            ++drained;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refresh) {
            const auto t0 = od::now_ns();
            Job job;
            job.chain = book->to_chain(source->currency());
            const auto snap_ns = od::now_ns() - t0;
            job.window = window;
            job.window_s = std::chrono::duration<double>(now - window_start).count();
            job.live = book->live_count();
            job.n_instruments = source->table().size();
            job.snapshot_us = snap_ns / 1e3;
            job.stats = source->stats();
            job.laps = source->laps();
            job.producer_stale = source->stale();
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
    source->stop();
    surface.stop();
    total_parse.merge(window.parse);
    total_hop.merge(window.hop);

    const auto stats = source->stats();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("\nrun summary: %.1fs, %llu updates received (%.0f/s), drops %llu, laps %llu, %s, %s\n",
                secs, static_cast<unsigned long long>(total_hop.count()), total_hop.count() / secs,
                static_cast<unsigned long long>(stats.ring_full),
                static_cast<unsigned long long>(source->laps()),
                args.bus.empty() ? "own feed" : "shared-memory bus",
                args.busy_poll ? "busy-poll" : "sleep-poll");
    print_latency("parse (recv->parsed)", total_parse);
    print_latency(args.bus.empty() ? "hop (parsed->pricer)" : "hop (feedd->this proc)", total_hop);
    print_latency("book snapshot", total_snapshot);
    print_latency("surface fit", surface.fit_hist());
    std::printf("  snapshots superseded before fitting: %llu\n",
                static_cast<unsigned long long>(surface.superseded()));
    std::printf("  exch->pricer p50 %.0f ms (includes local clock offset vs exchange)\n",
                us(exch_age.percentile(0.5)) / 1000.0);
    return 0;
}
