// od_gw_smoke: end-to-end check of the testnet order gateway, with play money.
//
//   source ~/.config/optionsdesk/testnet.env && od_gw_smoke
//
// 1. connect, authenticate, enable cancel-on-disconnect, check positions
// 2. option order at the instrument's minimum allowed price (can't fill), then
//    amend it, then cancel it: expect ack, amend ack, cancelled
// 3. post-only order priced through the market: expect a post-only reject
// 4. buy then sell $10 of the perpetual: two real fills with fees, net flat
// Every step reports its round-trip time. Leaves no orders and no position.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "od/deribit.hpp"
#include "od/deribit_gateway.hpp"
#include "od/quoter.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Waiter {
    od::deribit::DeribitGateway& gw;
    std::vector<od::ExecEvent> buf;

    // Wait up to `timeout` for an event of `kind` for `id`; prints what arrives.
    bool wait(std::uint64_t id, od::ExecEvent::Kind kind, double& ms, double timeout_s = 10.0) {
        const auto t0 = Clock::now();
        while (Clock::now() - t0 < std::chrono::duration<double>(timeout_s)) {
            buf.clear();
            gw.poll(buf);
            for (const auto& e : buf) {
                const char* k = e.kind == od::ExecEvent::Kind::Ack         ? "ack"
                                : e.kind == od::ExecEvent::Kind::Fill      ? "fill"
                                : e.kind == od::ExecEvent::Kind::Cancelled ? "cancelled"
                                                                            : "rejected";
                std::printf("    event: %-9s id=%llu qty=%g price=%g fee=%g %s\n", k,
                            static_cast<unsigned long long>(e.client_id), e.qty, e.price, e.fee,
                            e.reason.c_str());
                if (e.client_id == id && e.kind == kind) {
                    ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
};

int fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    return 1;
}

}  // namespace

int main() {
    const char* id = std::getenv("DERIBIT_CLIENT_ID");
    const char* secret = std::getenv("DERIBIT_CLIENT_SECRET");
    if (!id || !secret || !*id || !*secret) {
        std::puts("DERIBIT_CLIENT_ID / DERIBIT_CLIENT_SECRET not set (source ~/.config/optionsdesk/testnet.env)");
        return 2;
    }

    // Pick a liquid-ish testnet option: nearest expiry beyond a day, strike nearest the forward.
    std::string instrument;
    double min_price = 0.0, best_bid = 0.0, best_ask = 0.0;
    try {
        const auto snap = od::deribit::fetch_snapshot("BTC", /*testnet=*/true);
        const auto chain = od::deribit::to_chain(snap);
        for (const auto& slice : chain.expiries) {
            if (slice.T < 1.0 / 365) continue;
            double best = 1e18;
            for (const auto& q : slice.quotes)
                if (q.type == od::OptionType::Call && std::abs(q.strike - slice.forward) < best)
                    best = std::abs(q.strike - slice.forward), instrument = q.name;
            break;
        }
        const auto t = od::deribit::public_get("ticker?instrument_name=" + instrument, true);
        min_price = t.value("min_price", 0.0);
        best_bid = t.value("best_bid_price", 0.0);
        best_ask = t.value("best_ask_price", 0.0);
    } catch (const std::exception& e) {
        std::printf("FAIL: testnet market data: %s\n", e.what());
        return 1;
    }
    std::printf("instrument %s  min_price %g  bid %g  ask %g\n", instrument.c_str(), min_price, best_bid,
                best_ask);

    od::deribit::DeribitGateway gw({"BTC", id, secret});
    Waiter w{gw, {}};
    const auto t0 = Clock::now();
    while (!gw.ready() && Clock::now() - t0 < std::chrono::seconds(20))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!gw.ready()) {
        std::printf("FAIL: gateway not ready: %s\n", gw.last_error().c_str());
        return 1;
    }
    std::printf("1. connected and authenticated in %.0f ms; cancel-on-disconnect on\n",
                std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    const auto pos = gw.existing_positions();
    std::printf("   existing positions: %zu\n", pos.size());
    for (const auto& p : pos) std::printf("     %s %+g\n", p.instrument.c_str(), p.size);

    double ms = 0;
    std::printf("2. resting option order at the minimum price\n");
    gw.place({1, instrument, od::Side::Buy, min_price, 0.1, true});
    if (!w.wait(1, od::ExecEvent::Kind::Ack, ms)) return fail("no ack for place");
    std::printf("   placed: ack in %.1f ms\n", ms);
    const double amended = min_price + od::option_tick(min_price);
    gw.amend(1, amended, 0.1);
    if (!w.wait(1, od::ExecEvent::Kind::Ack, ms)) return fail("no ack for amend");
    std::printf("   amended to %g: ack in %.1f ms\n", amended, ms);
    gw.cancel(1);
    if (!w.wait(1, od::ExecEvent::Kind::Cancelled, ms)) return fail("no cancel confirmation");
    std::printf("   cancelled: confirmed in %.1f ms\n", ms);

    std::printf("3. post-only order through the market (should be rejected, not traded)\n");
    if (best_ask > 0.0) {
        gw.place({2, instrument, od::Side::Buy, best_ask, 0.1, true});
        if (!w.wait(2, od::ExecEvent::Kind::Rejected, ms)) return fail("crossing post-only order was not rejected");
        std::printf("   rejected in %.1f ms\n", ms);
    } else {
        std::printf("   skipped: no offer on the testnet book\n");
    }

    std::printf("4. perpetual round trip, $10 each way\n");
    const auto perp = od::deribit::public_get("ticker?instrument_name=BTC-PERPETUAL", true);
    const double px = perp.value("mark_price", 0.0);
    gw.place({3, "BTC-PERPETUAL", od::Side::Buy, px, 10, false});
    if (!w.wait(3, od::ExecEvent::Kind::Fill, ms)) return fail("perp buy did not fill");
    std::printf("   bought: filled in %.1f ms\n", ms);
    gw.place({4, "BTC-PERPETUAL", od::Side::Sell, px, 10, false});
    if (!w.wait(4, od::ExecEvent::Kind::Fill, ms)) return fail("perp sell did not fill");
    std::printf("   sold: filled in %.1f ms\n", ms);

    std::printf("PASS: gateway works end to end against %s\n", od::deribit::kTestnetHost);
    return 0;
}
