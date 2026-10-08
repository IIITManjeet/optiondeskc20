#include <gtest/gtest.h>

#include "od/implied_vol.hpp"

using od::IvStatus;
using od::OptionType;

TEST(ImpliedVol, RoundTripAcrossGrid) {
    const double F = 100.0;
    int checked = 0;
    for (double T : {1.0 / 365, 7.0 / 365, 0.25, 1.0, 3.0}) {
        for (double K : {40.0, 70.0, 90.0, 100.0, 110.0, 150.0, 250.0}) {
            for (double sigma : {0.05, 0.2, 0.6, 1.2, 2.5}) {
                for (auto type : {OptionType::Call, OptionType::Put}) {
                    const double px = od::black76_price(type, F, K, T, sigma);
                    const double intrinsic = std::max(type == OptionType::Call ? F - K : K - F, 0.0);
                    // Skip prices with no resolvable time value in double precision.
                    if (px - intrinsic < 1e-9 * F) continue;
                    const auto r = od::implied_vol(type, px, F, K, T);
                    ASSERT_TRUE(r.ok()) << "T=" << T << " K=" << K << " s=" << sigma << " "
                                        << od::to_string(r.status);
                    // Recovered price must match; sigma itself is only identifiable
                    // to the extent vega is non-zero.
                    EXPECT_NEAR(od::black76_price(type, F, K, T, r.sigma), px, 1e-9 * F);
                    if (od::black76_vega_raw(F, K, T, sigma) > 1e-3) {
                        EXPECT_NEAR(r.sigma, sigma, 1e-7);
                    }
                    ++checked;
                }
            }
        }
    }
    EXPECT_GT(checked, 250);
}

TEST(ImpliedVol, WithDiscounting) {
    const double px = od::black76_price(OptionType::Put, 100, 95, 0.5, 0.33, 0.05);
    const auto r = od::implied_vol(OptionType::Put, px, 100, 95, 0.5, 0.05);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.sigma, 0.33, 1e-9);
}

TEST(ImpliedVol, ConvergesQuickly) {
    const double px = od::black76_price(OptionType::Call, 100, 105, 0.25, 0.5);
    const auto r = od::implied_vol(OptionType::Call, px, 100, 105, 0.25);
    ASSERT_TRUE(r.ok());
    EXPECT_LE(r.iterations, 8);
}

TEST(ImpliedVol, RejectsArbitrageablePrices) {
    EXPECT_EQ(od::implied_vol(OptionType::Call, 9.0, 110, 100, 0.5).status, IvStatus::BelowIntrinsic);
    EXPECT_EQ(od::implied_vol(OptionType::Call, 120.0, 110, 100, 0.5).status, IvStatus::AboveMax);
    EXPECT_EQ(od::implied_vol(OptionType::Put, 100.0, 110, 100, 0.5).status, IvStatus::AboveMax);
    EXPECT_EQ(od::implied_vol(OptionType::Call, 1.0, 100, 100, 0.0).status, IvStatus::BadInput);
}
