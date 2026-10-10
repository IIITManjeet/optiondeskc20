// od_mm: options market maker on live market data.
//
// Paper trading on the simulated exchange, against real market data:
//   od_feedd --currency BTC
//   od_mm --config examples/mm_btc.json
//
// Real orders on Deribit TESTNET (play money; the gateway can't reach the real
// exchange), against testnet market data:
//   od_feedd --currency BTC --testnet
//   source ~/.config/optionsdesk/testnet.env
//   od_mm --gateway testnet --config examples/mm_btc_testnet.json
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

#include "od/deribit_gateway.hpp"
#include "od/feed_bus.hpp"
#include "od/mm_engine.hpp"
#include "od/surface.hpp"
#include "od/thread_util.hpp"

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_kill{false};
void on_signal(int sig) { (sig == SIGUSR1 ? g_kill : g_stop).store(true); }

struct Args {
    std::string bus;         // default od_feed_BTC, or od_feed_BTC_testnet with --gateway testnet
    std::string trades_bus;  // default od_trades_<CCY>[_testnet]
    std::string config = "examples/mm_btc.json";
    std::string gateway = "sim";
    int duration_s = 0;
    int status_ms = 5000;
    bool show_quotes = false;
    bool adopt_positions = false;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        if (f == "--show-quotes") {
            a.show_quotes = true;
            continue;
        }
        if (f == "--adopt-positions") {
            a.adopt_positions = true;
            continue;
        }
        const char* v = i + 1 < argc ? argv[++i] : nullptr;
        if (!v) return false;
        if (f == "--bus") a.bus = v;
        else if (f == "--trades-bus") a.trades_bus = v;
        else if (f == "--config") a.config = v;
        else if (f == "--gateway") a.gateway = v;
        else if (f == "--duration") a.duration_s = std::atoi(v);
        else if (f == "--status-ms") a.status_ms = std::atoi(v);
        else return false;
    }
    if (a.gateway != "sim" && a.gateway != "testnet") return false;
    const std::string suffix = a.gateway == "testnet" ? "_testnet" : "";
    if (a.bus.empty()) a.bus = "od_feed_BTC" + suffix;
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
            "usage: od_mm [--gateway sim|testnet] [--config FILE] [--duration S] [--status-ms N]\n"
            "             [--show-quotes] [--adopt-positions] [--bus SHM_NAME] [--trades-bus SHM_NAME]");
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
    const bool testnet = args.gateway == "testnet";

    // Trade prints drive the simulator's queue model; without them only book moves
    // can fill us. (On testnet the exchange decides fills, so they're informational.)
    std::optional<od::bus::TradeBusReader> trades;
    try {
        trades.emplace(od::bus::TradeBusReader::open(
            args.trades_bus.empty() ? "od_trades_" + ccy + (testnet ? "_testnet" : "") : args.trades_bus));
        trades->seek_to_end();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "warning: no trade feed (%s)\n", e.what());
    }

    od::name_current_thread("od-strategy");
    std::unique_ptr<od::Gateway> gateway;
    od::SimGateway* sim = nullptr;
    od::deribit::DeribitGateway* deribit = nullptr;
    if (testnet) {
        const char* id = std::getenv("DERIBIT_CLIENT_ID");
        const char* secret = std::getenv("DERIBIT_CLIENT_SECRET");
        if (!id || !secret || !*id || !*secret) {
            std::fprintf(stderr, "error: DERIBIT_CLIENT_ID / DERIBIT_CLIENT_SECRET not set "
                                 "(source ~/.config/optionsdesk/testnet.env)\n");
            return 1;
        }
        auto g = std::make_unique<od::deribit::DeribitGateway>(od::deribit::GatewayConfig{ccy, id, secret});
        deribit = g.get();
        gateway = std::move(g);
        const auto t0 = od::Clock::now();
        while (!(deribit->ready() && deribit->positions_checked()) && !g_stop &&
               od::Clock::now() - t0 < std::chrono::seconds(30))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!deribit->ready()) {
            std::fprintf(stderr, "error: testnet gateway not ready: %s\n", deribit->last_error().c_str());
            return 1;
        }
    } else {
        auto g = std::make_unique<od::SimGateway>();
        sim = g.get();
        gateway = std::move(g);
    }

    Fitter fitter;
    od::MarketMaker mm(cfg, reader->instruments(), ccy, *gateway, sim,
                       [&fitter](od::OptionChain c) { fitter.post(std::move(c)); }, stdout,
                       args.status_ms);
    mm.set_show_quotes(args.show_quotes);
    for (const auto& u : reader->snapshot()) mm.on_ticker(u);

    if (deribit) {
        // Positions we didn't create would be invisible to risk and hedging. Refuse
        // to trade on top of them unless told to take them over.
        const auto existing = deribit->existing_positions();
        for (const auto& p : existing)
            std::fprintf(stderr, "existing position: %s %+g @ %g\n", p.instrument.c_str(), p.size,
                         p.average_price);
        if (!existing.empty() && !args.adopt_positions) {
            std::fprintf(stderr, "error: the testnet account has positions. Close them, or pass "
                                 "--adopt-positions to include them in risk and hedging.\n");
            return 1;
        }
        for (const auto& p : existing) mm.adopt_position(p.instrument, p.size, p.average_price);
    }

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
            std::fprintf(stderr, "od_mm: quoting %zu %s options on %s\n", mm.quoted(), ccy.c_str(),
                         testnet ? "Deribit TESTNET" : "the simulated exchange");
        }
        if (args.duration_s > 0 && now - start >= std::chrono::seconds(args.duration_s)) break;
        if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    if (deribit) {
        // Cancel everything and wait (briefly) for the exchange to confirm, so the
        // final report reflects what is really still working.
        mm.kill();
        const auto t0 = od::Clock::now();
        while (!mm.orders().live_orders().empty() && od::Clock::now() - t0 < std::chrono::seconds(5)) {
            mm.on_timer(od::Clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!mm.orders().live_orders().empty())
            std::fprintf(stderr, "warning: %zu order(s) unconfirmed at exit; the gateway cancels all "
                                 "on close and the exchange cancels on disconnect\n",
                         mm.orders().live_orders().size());
    }
    mm.finish();
    return 0;
}
