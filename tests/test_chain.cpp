#include <gtest/gtest.h>

#include <filesystem>

#include "od/deribit.hpp"
#include "od/surface.hpp"

TEST(InstrumentName, ParsesInverseOption) {
    const auto id = od::parse_option_name("BTC-27NOV26-88000-C");
    ASSERT_TRUE(id);
    EXPECT_EQ(id->currency, "BTC");
    EXPECT_EQ(id->year, 2026);
    EXPECT_EQ(id->month, 11);
    EXPECT_EQ(id->day, 27);
    EXPECT_DOUBLE_EQ(id->strike, 88000.0);
    EXPECT_EQ(id->type, od::OptionType::Call);
}

TEST(InstrumentName, ParsesSingleDigitDayAndDecimalStrike) {
    const auto a = od::parse_option_name("ETH-9OCT26-2400-P");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->day, 9);
    EXPECT_EQ(a->type, od::OptionType::Put);

    const auto b = od::parse_option_name("XRP_USDC-27NOV26-0d625-C");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->currency, "XRP_USDC");
    EXPECT_DOUBLE_EQ(b->strike, 0.625);
}

TEST(InstrumentName, RejectsMalformed) {
    for (const char* bad : {"BTC-PERPETUAL", "BTC-27NOV26", "BTC-27XYZ26-1000-C",
                            "BTC-27NOV26-abc-C", "BTC-27NOV26-1000-X", "BTC-27NOV26-1000-C-1"})
        EXPECT_FALSE(od::parse_option_name(bad)) << bad;
}

TEST(Chain, GroupsByExpiryAndComputesForward) {
    std::vector<od::OptionQuote> q(3);
    q[0] = {"BTC-1JAN27-90000-C", od::OptionType::Call, 90000, 2000, 0, 0, 0, 0, 80000};
    q[1] = {"BTC-1JAN27-80000-P", od::OptionType::Put, 80000, 2000, 0, 0, 0, 0, 80010};
    q[2] = {"BTC-1JAN26-80000-P", od::OptionType::Put, 80000, 500, 0, 0, 0, 0, 1};  // expired
    const auto chain = od::build_chain("BTC", 1000, q);
    ASSERT_EQ(chain.expiries.size(), 1u);
    EXPECT_EQ(chain.expiries[0].label, "1JAN27");
    EXPECT_EQ(chain.expiries[0].quotes[0].strike, 80000);  // sorted by strike
    EXPECT_NEAR(chain.expiries[0].T, 1000.0 / od::kMsPerYear, 1e-18);
}

// End-to-end on a recorded snapshot (data/btc_snapshot.json), if present.
TEST(Surface, RecordedSnapshot) {
    const std::string path = std::string(OD_DATA_DIR) + "/btc_snapshot.json";
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "no recorded snapshot";

    const auto chain = od::deribit::to_chain(od::deribit::load_snapshot(path));
    ASSERT_GT(chain.expiries.size(), 3u);

    const auto check = od::check_against_exchange(chain);
    EXPECT_GT(check.n - check.n_low_vega, 100);
    EXPECT_EQ(check.iv_failures, 0);
    // mark_iv is published to 0.01 vol pts; the median should sit at rounding level.
    EXPECT_LE(check.median_iv_err, 0.0001);
    EXPECT_LE(check.max_iv_err, 0.005);

    const auto fits = od::build_surface(chain);
    ASSERT_GT(fits.size(), 3u);
    for (const auto& f : fits) {
        EXPECT_TRUE(f.svi.params.is_valid()) << f.slice->label;
        EXPECT_LT(f.rmse_vol, 0.03) << f.slice->label;  // within 3 vol pts of market mids
        EXPECT_GT(f.atm_iv, 0.05);
        EXPECT_LT(f.atm_iv, 3.0);
    }
}
