#pragma once
// Market-data types shared by the feed handler and the pricer.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "od/chain.hpp"

namespace od {

// One top-of-book + mark update for one option. Fixed size and trivially
// copyable so it can travel through the SPSC ring by value: no allocation and
// no pointers into the feed thread's buffers.
struct TickerUpdate {
    std::uint32_t instrument = 0;  // index into InstrumentTable
    std::int64_t exch_ts_ms = 0;   // exchange timestamp
    std::int64_t recv_ns = 0;      // steady clock: frame read off the socket
    std::int64_t parsed_ns = 0;    // steady clock: parsed, about to be pushed
    double bid = 0, ask = 0, bid_amount = 0, ask_amount = 0;
    double mark = 0, mark_iv = 0, underlying = 0, index_price = 0;
};

// One public trade. `taker_buy` = the aggressor bought (lifted offers); otherwise
// the aggressor sold (hit bids). Same transport rules as TickerUpdate.
struct TradeUpdate {
    std::uint32_t instrument = 0;
    std::uint32_t taker_buy = 0;
    std::int64_t exch_ts_ms = 0;
    std::int64_t recv_ns = 0;
    std::int64_t parsed_ns = 0;
    std::uint64_t trade_seq = 0;
    double price = 0, amount = 0, iv = 0, index_price = 0;
};

struct Instrument {
    std::string name;
    std::int64_t expiry_ms = 0;
    double strike = 0;
    OptionType type = OptionType::Call;
};

// Name <-> dense id. Built once at startup, then read-only, so both threads can use it.
class InstrumentTable {
public:
    std::uint32_t add(Instrument inst);
    std::optional<std::uint32_t> find(std::string_view name) const;
    const Instrument& operator[](std::uint32_t id) const { return list_[id]; }
    std::size_t size() const { return list_.size(); }
    const std::vector<Instrument>& all() const { return list_; }

private:
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    std::vector<Instrument> list_;
    std::unordered_map<std::string, std::uint32_t, Hash, std::equal_to<>> ids_;
};

// Latest state per instrument, owned by the pricer thread.
class LiveBook {
public:
    explicit LiveBook(const InstrumentTable& table);

    void apply(const TickerUpdate& u);
    std::size_t live_count() const { return live_; }
    std::int64_t last_exch_ts_ms() const { return last_ts_; }
    double index() const { return index_; }
    // Latest state of one instrument, or nullptr if it hasn't ticked yet.
    const OptionQuote* quote(std::uint32_t id) const { return seen_[id] ? &quotes_[id] : nullptr; }

    // Snapshot of every instrument that has received at least one update.
    OptionChain to_chain(const std::string& currency) const;

private:
    std::vector<OptionQuote> quotes_;
    std::vector<bool> seen_;
    std::size_t live_ = 0;
    std::int64_t last_ts_ = 0;
    double index_ = 0.0;
};

}  // namespace od
