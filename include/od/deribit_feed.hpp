#pragma once
// Deribit streaming feed handler.
//
// Runs on its own thread: TLS WebSocket to Deribit, subscribes to
// `ticker.{instrument}.100ms` for every option in the table, parses each
// notification into a TickerUpdate and pushes it into an SPSC ring for the pricer.
//
// Uses synchronous Boost.Beast: one connection, one thread, a blocking read loop.
// That's the simplest correct design for a single feed; an async io_context only
// pays off when one thread multiplexes many sockets. Reconnects with backoff on
// any error. interrupt() unblocks a pending read from another thread via
// shutdown(2) on the socket, so Ctrl-C exits promptly.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "od/market_data.hpp"
#include "od/spsc_ring.hpp"

namespace od::deribit {

// Written by the feed thread, read by anyone (relaxed: these are statistics).
struct FeedCounters {
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> tickers{0};
    std::atomic<std::uint64_t> ring_full{0};     // updates dropped because the pricer fell behind
    std::atomic<std::uint64_t> unknown{0};       // instruments not in the table (new listings)
    std::atomic<std::uint64_t> heartbeats{0};
    std::atomic<std::uint64_t> reconnects{0};
    std::atomic<std::uint64_t> subscribed{0};    // channels acknowledged by the exchange
    std::atomic<bool> connected{false};
};

struct FeedConfig {
    bool testnet = false;
    int heartbeat_s = 10;
    std::size_t subscribe_batch = 100;  // channels per public/subscribe request
};

class Feed {
public:
    Feed(FeedConfig cfg, const InstrumentTable& table, SpscRing<TickerUpdate>& ring,
         FeedCounters& counters);

    // Blocks until `stop` is set (and interrupt() called). Call on the feed thread.
    void run(const std::atomic<bool>& stop);

    // Thread-safe: unblocks a read in progress so run() can observe `stop`.
    void interrupt();

private:
    void session(const std::atomic<bool>& stop);

    FeedConfig cfg_;
    const InstrumentTable& table_;
    SpscRing<TickerUpdate>& ring_;
    FeedCounters& counters_;
    std::vector<std::string> channels_;
    std::atomic<int> fd_{-1};
};

}  // namespace od::deribit
