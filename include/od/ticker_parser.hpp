#pragma once
// Fast path for Deribit subscription notifications: `ticker.*` and `trades.*`.
//
// Uses simdjson On-Demand: no DOM is built, the parser walks the bytes and
// materialises only the fields we ask for. Anything else (RPC responses,
// heartbeats) is reported as Other so the caller can fall back to a general JSON
// parser; those messages are rare.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "od/market_data.hpp"

namespace od::deribit {

// instrument_name views are valid until the next parse() call. `instrument`,
// `recv_ns` and `parsed_ns` are left for the caller to fill in.
struct TickerFields {
    std::string_view instrument_name;
    TickerUpdate update;
};

struct TradeFields {
    std::string_view instrument_name;
    TradeUpdate update;
};

class TickerParser {
public:
    TickerParser();
    ~TickerParser();
    TickerParser(const TickerParser&) = delete;
    TickerParser& operator=(const TickerParser&) = delete;

    enum class Kind { Ticker, Trades, Other };

    // One pass over the frame. Ticker -> `tick` filled; Trades -> `trades` replaced
    // with every trade in the batch (Deribit sends them as an array).
    Kind parse(std::string_view frame, TickerFields& tick, std::vector<TradeFields>& trades);

    // Ticker-only convenience: true if `frame` was a ticker notification.
    bool parse(std::string_view frame, TickerFields& tick);

private:
    struct Impl;  // keeps simdjson out of this header
    std::unique_ptr<Impl> impl_;
};

}  // namespace od::deribit
