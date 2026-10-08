#include <gtest/gtest.h>

#include "od/histogram.hpp"

TEST(LatencyHistogram, EmptyIsZero) {
    od::LatencyHistogram h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.percentile(0.5), 0u);
}

TEST(LatencyHistogram, SmallValuesAreExact) {
    od::LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 10; ++v) h.record(v);
    EXPECT_EQ(h.percentile(0.0), 1u);
    EXPECT_EQ(h.percentile(0.5), 5u);
    EXPECT_EQ(h.percentile(1.0), 10u);
    EXPECT_EQ(h.max(), 10u);
}

TEST(LatencyHistogram, PercentilesWithinBucketError) {
    od::LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 100'000; ++v) h.record(v * 100);  // 100 ns .. 10 ms uniform
    for (double q : {0.5, 0.9, 0.99}) {
        const double exact = q * 100'000 * 100;
        const double got = static_cast<double>(h.percentile(q));
        EXPECT_GE(got, exact * 0.99) << q;
        EXPECT_LE(got, exact * 1.07) << q;  // 16 sub-buckets per octave => <= 1/16 high
    }
    EXPECT_EQ(h.max(), 10'000'000u);
}

TEST(LatencyHistogram, HandlesHugeValues) {
    od::LatencyHistogram h;
    h.record(~std::uint64_t{0});
    h.record(1);
    EXPECT_EQ(h.percentile(1.0), ~std::uint64_t{0});
}

TEST(LatencyHistogram, Merge) {
    od::LatencyHistogram a, b;
    for (int i = 0; i < 100; ++i) a.record(10), b.record(1000);
    a.merge(b);
    EXPECT_EQ(a.count(), 200u);
    EXPECT_EQ(a.percentile(0.25), 10u);
    EXPECT_GE(a.percentile(0.75), 1000u);
    EXPECT_EQ(a.max(), 1000u);
}
