#include <gtest/gtest.h>

#include <cmath>

#include "od/black76.hpp"
#include "od/web_snapshot.hpp"

namespace {

constexpr std::int64_t kAsof = 1'790'000'000'000;
constexpr std::int64_t kExpiry = kAsof + 30LL * 24 * 3600 * 1000;

// One 30-day expiry, F = 80000, a gently skewed smile; prices in the coin.
od::OptionChain chain() {
    const double F = 80000, T = 30.0 / 365;
    std::vector<od::OptionQuote> q;
    for (double K = 60000; K <= 100000; K += 2500) {
        const double k = std::log(K / F);
        const double iv = 0.5 - 0.1 * k + 0.4 * k * k;
        for (auto type : {od::OptionType::Call, od::OptionType::Put}) {
            const auto px = [&](double v) { return od::black76_price(type, F, K, T, v) / F; };
            od::OptionQuote x;
            x.name = "BTC-24NOV26-" + std::to_string(static_cast<int>(K)) + (type == od::OptionType::Call ? "-C" : "-P");
            x.type = type;
            x.strike = K;
            x.expiry_ms = kExpiry;
            x.bid = px(iv - 0.005);
            x.ask = px(iv + 0.005);
            x.mark = px(iv);
            x.mark_iv = iv;
            x.underlying = F;
            x.open_interest = 10;
            q.push_back(x);
        }
    }
    auto c = od::build_chain("BTC", kAsof, std::move(q));
    c.index_price = 79900;
    return c;
}

}  // namespace

TEST(WebSnapshot, WaitingForFeed) {
    od::web::SnapshotInput in;
    in.feed.bus = "od_feed_BTC";
    const auto j = od::web::snapshot_json(in, {}, 0, 123);
    EXPECT_EQ(j["type"], "snapshot");
    EXPECT_EQ(j["server_ms"], 123);
    EXPECT_TRUE(j["feed"].is_null());
    EXPECT_EQ(j["waiting_for"], "od_feed_BTC");
    EXPECT_FALSE(j.contains("expiries"));
}

TEST(WebSnapshot, ChainFitAndTrades) {
    od::web::SnapshotInput in;
    in.chain = chain();
    in.feed.bus = "od_feed_BTC";
    in.feed.stats.connected = true;
    in.trades.push_back({"BTC-24NOV26-80000-C", kAsof - 5, true, 0.05, 1.5, 0.5, 79900});
    const auto fits = od::build_surface(*in.chain);
    const auto j = od::web::snapshot_json(in, fits, 1.25, 456);

    EXPECT_EQ(j["currency"], "BTC");
    EXPECT_EQ(j["asof_ms"], kAsof);
    EXPECT_EQ(j["index"], 79900);
    EXPECT_EQ(j["fit_ms"], 1.25);
    EXPECT_EQ(j["feed"]["connected"], true);

    ASSERT_EQ(j["expiries"].size(), 1u);
    const auto& e = j["expiries"][0];
    EXPECT_FALSE(e["label"].get<std::string>().empty());
    EXPECT_NEAR(e["days"].get<double>(), 30.0, 1e-9);

    // Calls and puts paired on one row per strike.
    ASSERT_EQ(e["chain"].size(), 17u);
    for (const auto& row : e["chain"]) {
        ASSERT_TRUE(row["call"].is_object());
        ASSERT_TRUE(row["put"].is_object());
        const double cd = row["call"]["delta"], pd = row["put"]["delta"];
        EXPECT_NEAR(cd - pd, 1.0, 1e-9);  // put-call parity on delta, undiscounted
        EXPECT_GT(row["call"]["vega"].get<double>(), 0);
    }

    // The fit recovers the smile it was built from.
    ASSERT_TRUE(e.contains("fit"));
    EXPECT_NEAR(e["fit"]["atm_iv"].get<double>(), 0.5, 0.005);
    EXPECT_EQ(e["curve"].size(), 81u);
    EXPECT_FALSE(e["points"].empty());

    ASSERT_EQ(j["trades"].size(), 1u);
    EXPECT_EQ(j["trades"][0]["side"], "buy");
    EXPECT_EQ(j["trades"][0]["ts_ms"], kAsof - 5);
}
