#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "od/spsc_ring.hpp"

TEST(SpscRing, RejectsNonPowerOfTwo) {
    EXPECT_THROW(od::SpscRing<int>(6), std::invalid_argument);
    EXPECT_THROW(od::SpscRing<int>(1), std::invalid_argument);
}

TEST(SpscRing, FifoAndFullEmpty) {
    od::SpscRing<int> r(4);
    int v = 0;
    EXPECT_FALSE(r.try_pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(r.try_push(i));
    EXPECT_FALSE(r.try_push(99));  // full
    EXPECT_EQ(r.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(r.try_pop(v));
}

TEST(SpscRing, WrapsAround) {
    od::SpscRing<int> r(4);
    int v = 0;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(r.try_push(i));
        ASSERT_TRUE(r.try_push(i + 1));
        ASSERT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, i);
        ASSERT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, i + 1);
    }
}

// Producer and consumer on separate threads with a small ring so it is constantly
// full/empty. Every value must arrive exactly once and in order; with a broken
// memory ordering this fails (or trips ThreadSanitizer).
TEST(SpscRing, ConcurrentStress) {
    struct Msg {
        std::uint64_t seq;
        std::uint64_t check;  // derived from seq: catches torn / stale slot reads
    };
    constexpr std::uint64_t kN = 2'000'000;
    od::SpscRing<Msg> r(64);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kN;) {
            if (r.try_push({i, ~i})) ++i;
        }
    });

    std::uint64_t expected = 0;
    bool ok = true;
    Msg m{};
    while (expected < kN) {
        if (!r.try_pop(m)) continue;
        if (m.seq != expected || m.check != ~expected) {
            ok = false;
            break;
        }
        ++expected;
    }
    producer.join();
    EXPECT_TRUE(ok) << "out of order or corrupt at " << expected;
    EXPECT_EQ(expected, kN);
}
