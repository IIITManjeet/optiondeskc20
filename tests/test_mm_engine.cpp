#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "od/mm_engine.hpp"
#include "od/surface.hpp"

namespace {

constexpr std::int64_t kBaseMs = 1'790'000'000'000;           // exchange clock at start
constexpr std::int64_t kExpiryMs = kBaseMs + 30LL * 86'400'000;  // 30 days out
constexpr double kF = 80000.0;

struct Market {
    od::InstrumentTable table;
    std::vector<od::TickerUpdate> quotes;  // one per instrument
};

// One expiry, puts and calls 70k..90k, priced off a smile, one tick either side of theo.
Market make_market() {
    Market m;
    const double T = 30.0 / 365.0;
    for (int K = 70000; K <= 90000; K += 2000) {
        for (auto type : {od::OptionType::Put, od::OptionType::Call}) {
            const std::string name = "BTC-9NOV26-" + std::to_string(K) + (type == od::OptionType::Put ? "-P" : "-C");
            const auto id = m.table.add({name, kExpiryMs, double(K), type});
            const double k = std::log(K / kF);
            const double iv = 0.45 + 0.6 * k * k - 0.1 * k;
            const double theo = od::usd_to_coin(od::black76_price(type, kF, K, T, iv), kF);
            od::TickerUpdate u;
            u.instrument = id;
            u.bid = od::round_to_tick(theo - od::option_tick(theo), true);
            u.ask = od::round_to_tick(theo + od::option_tick(theo), false);
            u.bid_amount = u.ask_amount = 5.0;
            u.mark = theo;
            u.mark_iv = iv;
            u.underlying = u.index_price = kF;
            m.quotes.push_back(u);
        }
    }
    return m;
}

od::MmConfig config() {
    od::MmConfig c;
    c.expiries = {"9NOV26"};
    c.max_moneyness = 0.15;
    c.quote.half_spread_vol = 0.004;
    return c;
}

struct Outcome {
    std::uint64_t fills = 0, trades_quoted = 0, trades_at_quote = 0;
    double volume = 0, pnl = 0, edge = 0;
    std::size_t quoted = 0;
};

// 60 simulated seconds: the whole book every 100 ms, and every 5 s a seller hits
// the bid of each put, with enough size to get through the displayed queue.
Outcome run_once() {
    const auto m = make_market();
    od::SimGateway gw;
    std::FILE* sink = std::fopen("/dev/null", "w");
    std::unique_ptr<od::MarketMaker> mm;
    mm = std::make_unique<od::MarketMaker>(
        config(), m.table, "BTC", gw, &gw,
        [&mm](od::OptionChain chain) {
            const auto fits = od::build_surface(chain);
            mm->set_view(std::make_shared<const od::MarketView>(od::MarketView::build(chain, fits)));
        },
        sink, 0);

    const std::int64_t t0 = 1'000'000'000'000;  // steady-clock ns, arbitrary origin
    for (int step = 0; step <= 600; ++step) {
        const std::int64_t ns = t0 + step * 100'000'000LL;
        for (auto u : m.quotes) {
            u.exch_ts_ms = kBaseMs + step * 100;
            u.recv_ns = ns;
            mm->on_ticker(u);
        }
        if (step > 0 && step % 50 == 0) {
            for (const auto& u : m.quotes) {
                if (m.table[u.instrument].type != od::OptionType::Put) continue;
                od::TradeUpdate t;
                t.instrument = u.instrument;
                t.taker_buy = 0;
                t.price = u.bid;
                t.amount = 20.0;
                t.exch_ts_ms = kBaseMs + step * 100;
                mm->on_trade(t);
            }
        }
        mm->on_timer(od::Clock::time_point(std::chrono::nanoseconds(ns)));
    }
    mm->finish();
    std::fclose(sink);

    Outcome o;
    o.fills = mm->orders().fills();
    o.volume = mm->orders().volume();
    o.pnl = mm->pnl_coin();
    o.trades_quoted = mm->stats().trades_quoted;
    o.trades_at_quote = mm->stats().trades_at_quote;
    o.edge = mm->stats().edge_vol_qty;
    o.quoted = mm->quoted();
    return o;
}

}  // namespace

TEST(MmEngine, QuotesSelectedInstrumentsAndFillsFromTrades) {
    const auto o = run_once();
    EXPECT_GT(o.quoted, 5u);         // OTM options within 15% moneyness
    EXPECT_GT(o.trades_quoted, 0u);  // the engine saw trades in what it quotes
    EXPECT_GT(o.fills, 0u);          // and the queue model turned some into fills
    EXPECT_GT(o.volume, 0.0);
}

// The whole point of replay: same inputs, same decisions, bit for bit.
TEST(MmEngine, ReplayIsDeterministic) {
    const auto a = run_once();
    const auto b = run_once();
    EXPECT_EQ(a.fills, b.fills);
    EXPECT_EQ(a.trades_at_quote, b.trades_at_quote);
    EXPECT_EQ(a.volume, b.volume);
    EXPECT_EQ(a.pnl, b.pnl);
    EXPECT_EQ(a.edge, b.edge);
}
