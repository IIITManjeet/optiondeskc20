#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

#include "od/feed_bus.hpp"

namespace {

std::string unique_name() {
    static std::atomic<int> n{0};
    return "/od_test_" + std::to_string(::getpid()) + "_" + std::to_string(n++);
}

od::InstrumentTable small_table(int n = 3) {
    od::InstrumentTable t;
    for (int i = 0; i < n; ++i)
        t.add({"BTC-1JAN27-" + std::to_string(80000 + 1000 * i) + "-C", 1'800'000'000'000,
               80000.0 + 1000 * i, i % 2 ? od::OptionType::Put : od::OptionType::Call});
    return t;
}

// Every field derived from one sequence number, so a torn read (fields from two
// different writes) is detectable.
od::TickerUpdate make_update(std::uint64_t seq, std::uint32_t instrument) {
    od::TickerUpdate u;
    u.instrument = instrument;
    u.exch_ts_ms = static_cast<std::int64_t>(seq);
    u.recv_ns = static_cast<std::int64_t>(seq * 3);
    u.parsed_ns = static_cast<std::int64_t>(seq * 5);
    const double x = static_cast<double>(seq);
    u.bid = x, u.ask = x + 1, u.bid_amount = x + 2, u.ask_amount = x + 3;
    u.mark = x + 4, u.mark_iv = x + 5, u.underlying = x + 6, u.index_price = x + 7;
    return u;
}

bool consistent(const od::TickerUpdate& u) {
    const auto s = static_cast<std::uint64_t>(u.exch_ts_ms);
    const double x = static_cast<double>(s);
    return u.recv_ns == static_cast<std::int64_t>(s * 3) &&
           u.parsed_ns == static_cast<std::int64_t>(s * 5) && u.bid == x && u.ask == x + 1 &&
           u.bid_amount == x + 2 && u.ask_amount == x + 3 && u.mark == x + 4 &&
           u.mark_iv == x + 5 && u.underlying == x + 6 && u.index_price == x + 7;
}

}  // namespace

TEST(FeedBus, SegmentCarriesInstrumentTableAndCurrency) {
    const auto name = unique_name();
    const auto table = small_table(5);
    auto w = od::bus::FeedBusWriter::create(name, "BTC", table, 64);
    auto r = od::bus::FeedBusReader::open(name);

    EXPECT_EQ(r.currency(), "BTC");
    const auto t2 = r.instruments();
    ASSERT_EQ(t2.size(), table.size());
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        EXPECT_EQ(t2[i].name, table[i].name);
        EXPECT_EQ(t2[i].expiry_ms, table[i].expiry_ms);
        EXPECT_EQ(t2[i].strike, table[i].strike);
        EXPECT_EQ(t2[i].type, table[i].type);
    }
}

TEST(FeedBus, PollDeliversInOrder) {
    const auto name = unique_name();
    auto w = od::bus::FeedBusWriter::create(name, "BTC", small_table(), 64);
    auto r = od::bus::FeedBusReader::open(name);
    od::TickerUpdate u;
    EXPECT_EQ(r.poll(u), od::bus::FeedBusReader::Poll::Empty);
    for (std::uint64_t i = 1; i <= 10; ++i) w.publish(make_update(i, 0));
    for (std::uint64_t i = 1; i <= 10; ++i) {
        ASSERT_EQ(r.poll(u), od::bus::FeedBusReader::Poll::Update);
        EXPECT_EQ(u.exch_ts_ms, static_cast<std::int64_t>(i));
        EXPECT_TRUE(consistent(u));
    }
    EXPECT_EQ(r.poll(u), od::bus::FeedBusReader::Poll::Empty);
}

TEST(FeedBus, ReadersAreIndependent) {
    const auto name = unique_name();
    auto w = od::bus::FeedBusWriter::create(name, "BTC", small_table(), 64);
    auto a = od::bus::FeedBusReader::open(name);
    auto b = od::bus::FeedBusReader::open(name);
    for (std::uint64_t i = 1; i <= 5; ++i) w.publish(make_update(i, 0));
    od::TickerUpdate u;
    for (int i = 0; i < 5; ++i) ASSERT_EQ(a.poll(u), od::bus::FeedBusReader::Poll::Update);
    EXPECT_EQ(a.poll(u), od::bus::FeedBusReader::Poll::Empty);
    // b hasn't read anything yet; a draining the ring doesn't affect it.
    ASSERT_EQ(b.poll(u), od::bus::FeedBusReader::Poll::Update);
    EXPECT_EQ(u.exch_ts_ms, 1);
}

TEST(FeedBus, SlowReaderIsLappedAndResyncsFromLastValueCache) {
    const auto name = unique_name();
    auto w = od::bus::FeedBusWriter::create(name, "BTC", small_table(3), 16);
    auto r = od::bus::FeedBusReader::open(name);
    // 100 updates round-robin over 3 instruments into a 16-slot ring.
    for (std::uint64_t i = 1; i <= 100; ++i) w.publish(make_update(i, static_cast<std::uint32_t>(i % 3)));

    od::TickerUpdate u;
    EXPECT_EQ(r.poll(u), od::bus::FeedBusReader::Poll::Lapped);
    EXPECT_EQ(r.laps(), 1u);

    const auto snap = r.snapshot();
    ASSERT_EQ(snap.size(), 3u);
    for (const auto& s : snap) {
        EXPECT_TRUE(consistent(s));
        // Latest update for each instrument: 100 (i%3==1), 99 (0), 98 (2).
        const std::int64_t expect = s.instrument == 1 ? 100 : s.instrument == 0 ? 99 : 98;
        EXPECT_EQ(s.exch_ts_ms, expect) << "instrument " << s.instrument;
    }
    EXPECT_EQ(r.poll(u), od::bus::FeedBusReader::Poll::Empty);  // caught up after resync
    w.publish(make_update(101, 0));
    ASSERT_EQ(r.poll(u), od::bus::FeedBusReader::Poll::Update);
    EXPECT_EQ(u.exch_ts_ms, 101);
}

TEST(FeedBus, SnapshotSkipsInstrumentsThatNeverTicked) {
    const auto name = unique_name();
    auto w = od::bus::FeedBusWriter::create(name, "BTC", small_table(4), 16);
    w.publish(make_update(7, 2));
    auto r = od::bus::FeedBusReader::open(name);
    const auto snap = r.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].instrument, 2u);
}

// Writer and reader on separate threads with a tiny ring so the reader is lapped
// constantly. The reader must never accept a torn update, and accepted updates
// must be strictly increasing.
TEST(FeedBus, ConcurrentWriterNeverYieldsTornReads) {
    const auto name = unique_name();
    auto w = od::bus::FeedBusWriter::create(name, "BTC", small_table(4), 8);
    auto r = od::bus::FeedBusReader::open(name);
    constexpr std::uint64_t kN = 1'000'000;

    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (std::uint64_t i = 1; i <= kN; ++i) w.publish(make_update(i, static_cast<std::uint32_t>(i % 4)));
        done = true;
    });

    std::uint64_t accepted = 0, torn = 0, last = 0, regress = 0;
    od::TickerUpdate u;
    for (;;) {
        const bool finished = done.load();
        const auto p = r.poll(u);
        if (p == od::bus::FeedBusReader::Poll::Empty && finished) break;  // fully drained
        switch (p) {
            case od::bus::FeedBusReader::Poll::Update: {
                if (!consistent(u)) ++torn;
                const auto s = static_cast<std::uint64_t>(u.exch_ts_ms);
                if (s <= last) ++regress;
                last = s;
                ++accepted;
                break;
            }
            case od::bus::FeedBusReader::Poll::Lapped:
                for (const auto& s : r.snapshot()) torn += !consistent(s);
                last = 0;  // snapshot entries may precede what we already saw
                break;
            case od::bus::FeedBusReader::Poll::Empty:
                break;
        }
    }
    writer.join();
    EXPECT_EQ(torn, 0u);
    EXPECT_EQ(regress, 0u);
    EXPECT_GT(accepted, 0u);
}

// Real cross-process use: the child process attaches by name and reads what the
// parent publishes.
TEST(FeedBus, WorksAcrossProcesses) {
    const auto name = unique_name();
    auto w = od::bus::FeedBusWriter::create(name, "ETH", small_table(2), 1024);
    for (std::uint64_t i = 1; i <= 3; ++i) w.publish(make_update(i, 1));

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        int code = 0;
        try {
            auto r = od::bus::FeedBusReader::open(name);
            const auto snap = r.snapshot();  // state published before we attached
            if (r.currency() != "ETH" || snap.size() != 1 || snap[0].exch_ts_ms != 3) code = 1;
            // ...then live updates from the parent.
            od::TickerUpdate u;
            for (std::uint64_t want = 4; want <= 503 && code == 0;) {
                const auto p = r.poll(u);
                if (p == od::bus::FeedBusReader::Poll::Update) {
                    if (u.exch_ts_ms != static_cast<std::int64_t>(want) || !consistent(u)) code = 2;
                    ++want;
                } else if (p == od::bus::FeedBusReader::Poll::Lapped) {
                    code = 3;
                }
            }
        } catch (...) {
            code = 4;
        }
        ::_exit(code);
    }
    ::usleep(50'000);  // let the child attach and snapshot before streaming
    for (std::uint64_t i = 4; i <= 503; ++i) w.publish(make_update(i, 1));
    int status = 0;
    ::waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(FeedBus, WriterUnlinksOnDestruction) {
    const auto name = unique_name();
    { auto w = od::bus::FeedBusWriter::create(name, "BTC", small_table(), 16); }
    EXPECT_THROW(od::bus::FeedBusReader::open(name), std::runtime_error);
}

TEST(FeedBus, RejectsBadCapacity) {
    EXPECT_THROW(od::bus::FeedBusWriter::create(unique_name(), "BTC", small_table(), 12),
                 std::invalid_argument);
}
