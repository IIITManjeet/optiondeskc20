#include <gtest/gtest.h>

#include "od/black76.hpp"

using od::OptionType;

TEST(Black76, NormCdf) {
    EXPECT_NEAR(od::norm_cdf(0.0), 0.5, 1e-15);
    EXPECT_NEAR(od::norm_cdf(1.96), 0.9750021048517795, 1e-12);
    EXPECT_NEAR(od::norm_cdf(-8.0), 6.22096057427178e-16, 1e-25);
}

TEST(Black76, PutCallParity) {
    // C - P = df * (F - K) for any vol.
    for (double K : {50.0, 90.0, 100.0, 130.0}) {
        for (double sigma : {0.05, 0.4, 1.5}) {
            const double F = 100.0, T = 0.75, r = 0.03;
            const double c = od::black76_price(OptionType::Call, F, K, T, sigma, r);
            const double p = od::black76_price(OptionType::Put, F, K, T, sigma, r);
            EXPECT_NEAR(c - p, std::exp(-r * T) * (F - K), 1e-10);
        }
    }
}

TEST(Black76, GreeksMatchFiniteDifferences) {
    const double F = 81000.0, K = 88000.0, T = 0.2, s = 0.45, r = 0.02;
    for (auto type : {OptionType::Call, OptionType::Put}) {
        const auto g = od::black76_greeks(type, F, K, T, s, r);
        auto px = [&](double f, double t, double v) { return od::black76_price(type, f, K, t, v, r); };
        const double hF = 1.0, hs = 1e-5, hT = 1e-6;
        EXPECT_NEAR(g.price, px(F, T, s), 1e-9);
        EXPECT_NEAR(g.delta, (px(F + hF, T, s) - px(F - hF, T, s)) / (2 * hF), 1e-6);
        EXPECT_NEAR(g.gamma, (px(F + hF, T, s) - 2 * px(F, T, s) + px(F - hF, T, s)) / (hF * hF), 1e-7);
        EXPECT_NEAR(g.vega, (px(F, T, s + hs) - px(F, T, s - hs)) / (2 * hs) / 100.0, 1e-4);
        EXPECT_NEAR(g.theta, -(px(F, T + hT, s) - px(F, T - hT, s)) / (2 * hT) / 365.0, 1e-3);
    }
}

// One ticker snapshot from Deribit's public API (2026-10-08, BTC-27NOV26-88000-C):
//   underlying_price 82022.7 (forward), mark_iv 37.22%, interest_rate 0,
//   T = (expiry - ticker timestamp) = 0.13580176201801114y,
//   mark_price 0.0275 (rounded to the 0.0005 tick), delta 0.32843,
//   vega 109.25415, theta -41.01911.
// Our Black-76 reproduces them to the precision Deribit publishes.
TEST(Black76, MatchesDeribitConventions) {
    const double F = 82022.7, K = 88000.0, T = 0.13580176201801114, s = 0.3722;
    const auto g = od::black76_greeks(OptionType::Call, F, K, T, s);
    EXPECT_NEAR(od::usd_to_coin(g.price, F), 0.0275, 2.5e-4);
    EXPECT_NEAR(g.delta, 0.32843, 5e-5);
    EXPECT_NEAR(g.vega, 109.25415, 1e-2);
    EXPECT_NEAR(g.theta, -41.01911, 1e-2);
}

TEST(Black76, ExpiredIsIntrinsic) {
    EXPECT_DOUBLE_EQ(od::black76_price(OptionType::Call, 110, 100, 0.0, 0.5), 10.0);
    EXPECT_DOUBLE_EQ(od::black76_price(OptionType::Put, 110, 100, 0.0, 0.5), 0.0);
}
