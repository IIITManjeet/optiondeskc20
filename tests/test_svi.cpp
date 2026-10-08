#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "od/svi.hpp"

TEST(Svi, DerivativesMatchFiniteDifferences) {
    const od::SviParams p{0.02, 0.1, -0.4, 0.05, 0.2};
    const double h = 1e-5;
    for (double k : {-1.0, -0.2, 0.0, 0.3, 1.2}) {
        EXPECT_NEAR(p.w1(k), (p.w(k + h) - p.w(k - h)) / (2 * h), 1e-8);
        EXPECT_NEAR(p.w2(k), (p.w(k + h) - 2 * p.w(k) + p.w(k - h)) / (h * h), 1e-4);
    }
}

TEST(Svi, RecoversKnownSmile) {
    const od::SviParams truth{0.01, 0.12, -0.35, 0.03, 0.15};
    std::vector<od::SviQuote> q;
    for (int i = 0; i <= 30; ++i) {
        const double k = -0.8 + 1.4 * i / 30;
        q.push_back({k, truth.w(k), 1.0});
    }
    const auto fit = od::fit_svi(q);
    ASSERT_TRUE(fit.ok);
    EXPECT_LT(fit.rmse_w, 1e-7);
    for (const auto& e : q) EXPECT_NEAR(fit.params.w(e.k), e.w, 1e-6);
}

TEST(Svi, RobustToNoise) {
    const od::SviParams truth{0.02, 0.2, -0.6, 0.0, 0.25};
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 0.0005);
    std::vector<od::SviQuote> q;
    for (int i = 0; i <= 40; ++i) {
        const double k = -1.0 + 1.6 * i / 40;
        q.push_back({k, truth.w(k) + noise(rng), 1.0});
    }
    const auto fit = od::fit_svi(q);
    ASSERT_TRUE(fit.ok);
    EXPECT_LT(fit.rmse_w, 0.001);
    EXPECT_NEAR(fit.params.w(0.0), truth.w(0.0), 0.001);
}

TEST(Svi, TooFewPointsIsNotOk) {
    std::vector<od::SviQuote> q{{0.0, 0.04, 1.0}, {0.1, 0.05, 1.0}};
    EXPECT_FALSE(od::fit_svi(q).ok);
}

TEST(Svi, DurrlemanDetectsButterflyArbitrage) {
    // Axel Vogt's example (Gatheral & Jacquier, "Arbitrage-free SVI volatility surfaces"):
    // satisfies the basic parameter constraints but has negative density near k ~ 0.6.
    const od::SviParams vogt{-0.0410, 0.1331, 0.3060, 0.3586, 0.4153};
    EXPECT_LT(od::min_durrleman_g(vogt), 0.0);

    const od::SviParams sane{0.02, 0.1, -0.4, 0.05, 0.2};
    EXPECT_GT(od::min_durrleman_g(sane), 0.0);
}
