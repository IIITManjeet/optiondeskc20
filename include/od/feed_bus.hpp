#pragma once
// Shared-memory market-data bus: one feed process, any number of reader processes.
// One segment per message type ("topic"): od_feedd publishes tickers to
// /dev/shm/od_feed_<CCY> and trades to /dev/shm/od_trades_<CCY>.
//
// Segment layout (/dev/shm/<name>):
//
//   Header             magic, payload type, geometry, currency, write_seq,
//                      heartbeat, feed stats
//   InstrumentRecord[] the instrument table, so readers need no REST call
//   LvcSlot[]          last-value cache: latest message per instrument
//   Slot[]             broadcast ring, capacity a power of two
//
// The ring is single-writer / multi-reader and the writer never waits: each reader
// keeps a private read position, and a reader that falls more than `capacity`
// behind is *lapped*. For tickers it then resyncs from the last-value cache, which
// is safe because tickers are full-state updates: only intermediate states are
// skipped, never the latest one. (Trades are events, not state: a lapped trade
// reader has genuinely missed trades and can only count them.)
//
// Every slot is a seqlock. The writer marks the slot busy, writes the payload,
// then publishes the slot's sequence; a reader copies the payload between two
// reads of that sequence and discards the copy if it changed. Payload words are
// std::atomic<uint64_t> with relaxed ordering, so concurrent access is well-defined
// C++ (a plain memcpy would be a data race). Lock-free 64-bit atomics are
// address-free, so they work across processes in a shared mapping.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "od/deribit_feed.hpp"
#include "od/market_data.hpp"

namespace od::bus {

inline constexpr std::uint64_t kMagic = 0x4F44464545444255ULL;  // "ODFEEDBU"
inline constexpr std::uint32_t kVersion = 2;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

// Payload types the bus can carry, each with a tag stored in the segment header.
template <class T> struct PayloadTag;
template <> struct PayloadTag<TickerUpdate> { static constexpr std::uint32_t value = 1; };
template <> struct PayloadTag<TradeUpdate> { static constexpr std::uint32_t value = 2; };

struct Header;
template <class T> struct Slot;
template <class T> struct LvcSlot;

// RAII for a shm_open + mmap mapping.
class Mapping {
public:
    Mapping() = default;
    Mapping(void* addr, std::size_t len) : addr_(addr), len_(len) {}
    Mapping(Mapping&& o) noexcept;
    Mapping& operator=(Mapping&& o) noexcept;
    ~Mapping();
    void* addr() const { return addr_; }

private:
    void* addr_ = nullptr;
    std::size_t len_ = 0;
};

template <class T>
class Writer {
public:
    // Creates (replacing any stale segment of the same name) and initialises the segment.
    static Writer create(const std::string& name, const std::string& currency,
                         const InstrumentTable& table, std::size_t capacity_pow2);
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) = delete;
    ~Writer();  // unlinks the segment; attached readers notice the heartbeat stop

    // Single writer thread only.
    void publish(const T& msg);

    // Any one thread (may differ from the publishing thread).
    void heartbeat(std::int64_t now_ns);
    void set_stats(const deribit::FeedStats& s);

    std::uint64_t published() const { return seq_; }

private:
    Writer() = default;
    std::string name_;
    Mapping map_;
    Header* hdr_ = nullptr;
    Slot<T>* slots_ = nullptr;
    LvcSlot<T>* lvc_ = nullptr;
    std::uint64_t mask_ = 0;
    std::uint64_t seq_ = 0;
};

template <class T>
class Reader {
public:
    static Reader open(const std::string& name);

    std::string currency() const;
    InstrumentTable instruments() const;
    std::int64_t epoch() const;  // producer start time; changes if the producer restarts
    std::int64_t heartbeat_ns() const;
    deribit::FeedStats stats() const;

    // Latest message of every instrument that has one. Also moves the read position
    // to "now": call on attach and after Poll::Lapped. Stream messages that arrive
    // during the copy may repeat state already in the snapshot; for full-state
    // tickers applying them again is harmless.
    std::vector<T> snapshot();

    // Skip to the newest message without reading the cache (for event streams).
    void seek_to_end();

    enum class Poll { Update, Empty, Lapped };
    Poll poll(T& out);

    std::uint64_t laps() const { return laps_; }
    std::uint64_t position() const { return pos_; }

private:
    Reader() = default;
    Mapping map_;
    const Header* hdr_ = nullptr;
    const Slot<T>* slots_ = nullptr;
    const LvcSlot<T>* lvc_ = nullptr;
    std::uint64_t mask_ = 0;
    std::uint64_t pos_ = 0;
    std::uint64_t laps_ = 0;
};

using FeedBusWriter = Writer<TickerUpdate>;
using FeedBusReader = Reader<TickerUpdate>;
using TradeBusWriter = Writer<TradeUpdate>;
using TradeBusReader = Reader<TradeUpdate>;

}  // namespace od::bus
