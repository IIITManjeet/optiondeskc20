#pragma once
// Market-data journal: a binary, append-only recording of tickers and trades.
//
// File layout:
//   FileHeader
//   JournalInstrument[n_instruments]       the instrument table at record time
//   { RecordHeader, payload }*             in arrival order
//
// Records carry the feed's local receive time (steady clock, ns), so a replay can
// reproduce the exact interleaving and spacing of what the live system saw. The
// writer goes through a large stdio buffer (a few hundred KB/s at most here, so
// no need for anything cleverer) and fsyncs on close. The reader maps the file
// read-only and stops cleanly at a truncated final record, which is what a crash
// mid-write leaves behind.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "od/feed_bus.hpp"

namespace od::journal {

inline constexpr char kMagic[8] = {'O', 'D', 'J', 'R', 'N', 'L', '0', '1'};
inline constexpr std::uint32_t kVersion = 1;

enum class RecordType : std::uint16_t { Ticker = 1, Trade = 2 };

struct FileHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t n_instruments;
    char currency[16];
    std::int64_t start_unix_ms;
};

struct JournalInstrument {
    char name[48];
    std::int64_t expiry_ms;
    double strike;
    std::uint64_t is_put;
};

struct RecordHeader {
    std::uint16_t type;
    std::uint16_t size;  // payload bytes
    std::uint32_t reserved;
    std::int64_t ts_ns;  // feed receive time, steady clock
};

class Writer {
public:
    Writer(const std::string& path, const std::string& currency, const InstrumentTable& table);
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();  // flushes and fsyncs

    void write(const TickerUpdate& u) { append(RecordType::Ticker, u.recv_ns, &u, sizeof u); }
    void write(const TradeUpdate& t) { append(RecordType::Trade, t.recv_ns, &t, sizeof t); }
    void flush();

    std::uint64_t records() const { return records_; }
    std::uint64_t bytes() const { return bytes_; }

private:
    void append(RecordType type, std::int64_t ts, const void* payload, std::size_t size);
    std::FILE* f_ = nullptr;
    std::vector<char> buf_;
    std::uint64_t records_ = 0, bytes_ = 0;
};

// One decoded record. Exactly one of ticker/trade is meaningful, per `type`.
struct Event {
    RecordType type;
    std::int64_t ts_ns;
    TickerUpdate ticker;
    TradeUpdate trade;
};

class Reader {
public:
    explicit Reader(const std::string& path);

    const std::string& currency() const { return currency_; }
    const InstrumentTable& instruments() const { return table_; }
    std::int64_t start_unix_ms() const { return start_unix_ms_; }

    // Next record in file order; false at the end (or at a truncated tail).
    bool next(Event& out);
    void rewind() { pos_ = first_record_; }
    bool truncated() const { return truncated_; }

private:
    bus::Mapping map_;
    std::size_t len_ = 0, first_record_ = 0, pos_ = 0;
    std::string currency_;
    InstrumentTable table_;
    std::int64_t start_unix_ms_ = 0;
    bool truncated_ = false;
};

}  // namespace od::journal
