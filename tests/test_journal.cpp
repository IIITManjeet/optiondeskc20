#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "od/journal.hpp"

namespace {

std::string temp_path(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            ("od_journal_" + std::to_string(::getpid()) + "_" + tag + ".odj"))
        .string();
}

od::InstrumentTable table() {
    od::InstrumentTable t;
    t.add({"BTC-27NOV26-88000-C", 1795766400000, 88000, od::OptionType::Call});
    t.add({"BTC-27NOV26-70000-P", 1795766400000, 70000, od::OptionType::Put});
    return t;
}

}  // namespace

TEST(Journal, RoundTripPreservesOrderAndContent) {
    const auto path = temp_path("rt");
    {
        od::journal::Writer w(path, "BTC", table());
        for (int i = 0; i < 100; ++i) {
            if (i % 3 == 0) {
                od::TradeUpdate t;
                t.instrument = 1;
                t.recv_ns = 1000 + i;
                t.trade_seq = static_cast<std::uint64_t>(i);
                t.price = 0.01 * i;
                w.write(t);
            } else {
                od::TickerUpdate u;
                u.instrument = 0;
                u.recv_ns = 1000 + i;
                u.bid = 0.001 * i;
                w.write(u);
            }
        }
        EXPECT_EQ(w.records(), 100u);
    }
    od::journal::Reader r(path);
    EXPECT_EQ(r.currency(), "BTC");
    ASSERT_EQ(r.instruments().size(), 2u);
    EXPECT_EQ(r.instruments()[1].name, "BTC-27NOV26-70000-P");
    EXPECT_EQ(r.instruments()[1].type, od::OptionType::Put);

    od::journal::Event e;
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(r.next(e)) << i;
        EXPECT_EQ(e.ts_ns, 1000 + i);
        if (i % 3 == 0) {
            ASSERT_EQ(e.type, od::journal::RecordType::Trade);
            EXPECT_EQ(e.trade.trade_seq, static_cast<std::uint64_t>(i));
            EXPECT_DOUBLE_EQ(e.trade.price, 0.01 * i);
        } else {
            ASSERT_EQ(e.type, od::journal::RecordType::Ticker);
            EXPECT_DOUBLE_EQ(e.ticker.bid, 0.001 * i);
        }
    }
    EXPECT_FALSE(r.next(e));
    EXPECT_FALSE(r.truncated());
    r.rewind();
    ASSERT_TRUE(r.next(e));
    EXPECT_EQ(e.ts_ns, 1000);
    std::filesystem::remove(path);
}

// A crash mid-write leaves a partial last record: everything before it is readable.
TEST(Journal, TruncatedTailIsIgnored) {
    const auto path = temp_path("trunc");
    {
        od::journal::Writer w(path, "BTC", table());
        od::TickerUpdate u;
        for (int i = 0; i < 5; ++i) {
            u.recv_ns = i;
            w.write(u);
        }
    }
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 10);
    od::journal::Reader r(path);
    od::journal::Event e;
    int n = 0;
    while (r.next(e)) ++n;
    EXPECT_EQ(n, 4);
    EXPECT_TRUE(r.truncated());
    std::filesystem::remove(path);
}

TEST(Journal, RejectsNonJournalFiles) {
    const auto path = temp_path("bad");
    {
        std::ofstream f(path);
        f << std::string(200, 'x');
    }
    EXPECT_THROW(od::journal::Reader{path}, std::runtime_error);
    std::filesystem::remove(path);
}
