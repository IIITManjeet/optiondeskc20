// od_mm: options market maker, paper trading against live market data.
//
//   od_feedd --currency BTC                                    (terminal 1)
//   od_mm --bus od_feed_BTC --config examples/mm_btc.json      (terminal 2)
//
// Live driver for the MarketMaker engine: feeds it tickers and trades from the
// shared-memory buses and the steady clock, runs SVI refits on a separate thread
// (latest-wins) and hands finished surfaces back. The strategy itself lives in
// src/mm_engine.cpp and is shared with od_replay.
//
// Ctrl-C cancels everything and prints a summary. SIGUSR1 trips the kill switch:
// cancel all, reject every new order, keep reporting.

#include <signal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "od/feed_bus.hpp"
#include "od/mm_engine.hpp"
#include "od/surface.hpp"
#include "od/thread_util.hpp"

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_kill{false};
void on_signal(int sig) { (sig == SIGUSR1 ? g_kill : g_stop).store(true); }

struct Args {
    std::string bus = "od_feed_BTC";
    std::string trades_bus;  // default od_trades_<CCY>
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
        else if (f == "--trades-bus") a.trades_bus = v;
        else if (f == "--config") a.config = v;
        else if (f == "--duration") a.duration_s = std::atoi(v);
        else if (f == "--status-ms") a.status_ms = std::atoi(v);
        else return false;
    }
    return true;
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

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::puts(
            "usage: od_mm [--bus SHM_NAME] [--trades-bus SHM_NAME] [--config FILE] [--duration S]\n"
            "             [--status-ms N] [--show-quotes]");
        return 2;
    }
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);

    od::MmConfig cfg;
    std::optional<od::bus::FeedBusReader> reader;
    try {
        cfg = od::load_mm_config(args.config);
        reader.emplace(od::bus::FeedBusReader::open(args.bus));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    const auto ccy = reader->currency();

    // Trade prints drive the queue model; without them only book moves can fill us.
    std::optional<od::bus::TradeBusReader> trades;
    try {
        trades.emplace(od::bus::TradeBusReader::open(args.trades_bus.empty() ? "od_trades_" + ccy
                                                                             : args.trades_bus));
        trades->seek_to_end();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "warning: no trade feed (%s); fills only from book moves\n", e.what());
    }

    od::name_current_thread("od-strategy");
    Fitter fitter;
    od::SimGateway gw;
    od::MarketMaker mm(cfg, reader->instruments(), ccy, gw, &gw,
                       [&fitter](od::OptionChain c) { fitter.post(std::move(c)); }, stdout,
                       args.status_ms);
    mm.set_show_quotes(args.show_quotes);
    for (const auto& u : reader->snapshot()) mm.on_ticker(u);

    bool announced = false;
    const auto start = od::Clock::now();
    while (!g_stop.load(std::memory_order_relaxed)) {
        int n = 0;
        od::TickerUpdate u;
        for (;;) {
            const auto p = reader->poll(u);
            if (p == od::bus::FeedBusReader::Poll::Empty) break;
            if (p == od::bus::FeedBusReader::Poll::Lapped) {
                for (const auto& s : reader->snapshot()) mm.on_ticker(s);
                continue;
            }
            mm.on_ticker(u);
            ++n;
        }
        if (trades) {
            od::TradeUpdate t;
            for (;;) {
                const auto p = trades->poll(t);
                if (p == od::bus::TradeBusReader::Poll::Empty) break;
                if (p == od::bus::TradeBusReader::Poll::Lapped) {
                    trades->seek_to_end();  // events, not state: missed trades are gone
                    continue;
                }
                mm.on_trade(t);
                ++n;
            }
        }
        if (g_kill) mm.kill();
        if (const auto v = fitter.latest()) {
            mm.set_view(v);
            if (mm.quoted() == 0) {
                std::fprintf(stderr, "nothing to quote: check \"expiries\" in %s\n", args.config.c_str());
                return 1;
            }
        }
        const auto now = od::Clock::now();
        mm.on_timer(now);

        if (!announced && mm.ready()) {
            announced = true;
            std::fprintf(stderr, "od_mm: paper trading %zu %s options on the simulated exchange\n",
                         mm.quoted(), ccy.c_str());
        }
        if (args.duration_s > 0 && now - start >= std::chrono::seconds(args.duration_s)) break;
        if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    mm.finish();
    return 0;
}
