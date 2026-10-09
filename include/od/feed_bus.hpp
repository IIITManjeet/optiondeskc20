#pragma once
// Shared-memory market-data bus: one feed process, any number of reader processes.
//
// Segment layout (/dev/shm/<name>):
//
//   Header             magic, geometry, currency, write_seq, heartbeat, feed stats
//   InstrumentRecord[] the instrument table, so readers need no REST call
//   LvcSlot[]          last-value cache: latest update per instrument
//   Slot[]             broadcast ring of updates, capacity a power of two
//
// The ring is single-writer / multi-reader and the writer never waits: each reader
// keeps a private read position, and a reader that falls more than `capacity`
// behind is *lapped*. It then resyncs from the last-value cache, which is safe
// because tickers are full-state updates: only intermediate states are skipped,
// never the latest one.
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
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kPayloadWords = sizeof(TickerUpdate) / 8;
static_assert(sizeof(TickerUpdate) % 8 == 0);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

struct Header;
struct Slot;
struct LvcSlot;

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

class FeedBusWriter {
public:
    // Creates (replacing any stale segment of the same name) and initialises the segment.
    static FeedBusWriter create(const std::string& name, const std::string& currency,
                                const InstrumentTable& table, std::size_t capacity_pow2);
    FeedBusWriter(FeedBusWriter&&) noexcept;
    FeedBusWriter& operator=(FeedBusWriter&&) = delete;
    ~FeedBusWriter();  // unlinks the segment; attached readers notice the heartbeat stop

    // Single writer thread only.
    void publish(const TickerUpdate& u);

    // Any one thread (may differ from the publishing thread).
    void heartbeat(std::int64_t now_ns);
    void set_stats(const deribit::FeedStats& s);

    std::uint64_t published() const { return seq_; }

private:
    FeedBusWriter() = default;
    std::string name_;
    Mapping map_;
    Header* hdr_ = nullptr;
    Slot* slots_ = nullptr;
    LvcSlot* lvc_ = nullptr;
    std::uint64_t mask_ = 0;
    std::uint64_t seq_ = 0;
};

class FeedBusReader {
public:
    static FeedBusReader open(const std::string& name);

    std::string currency() const;
    InstrumentTable instruments() const;
    std::int64_t epoch() const;  // producer start time; changes if the producer restarts
    std::int64_t heartbeat_ns() const;
    deribit::FeedStats stats() const;

    // Latest update of every instrument that has ticked. Also moves the read position
    // to "now": call on attach and after Poll::Lapped. Stream updates that arrive
    // during the copy may repeat state already in the snapshot; applying them again
    // is harmless because each update carries full state.
    std::vector<TickerUpdate> snapshot();

    enum class Poll { Update, Empty, Lapped };
    Poll poll(TickerUpdate& out);

    std::uint64_t laps() const { return laps_; }
    std::uint64_t position() const { return pos_; }

private:
    FeedBusReader() = default;
    Mapping map_;
    const Header* hdr_ = nullptr;
    const Slot* slots_ = nullptr;
    const LvcSlot* lvc_ = nullptr;
    std::uint64_t mask_ = 0;
    std::uint64_t pos_ = 0;
    std::uint64_t laps_ = 0;
};

}  // namespace od::bus
