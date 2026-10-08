#pragma once
// Fast path for Deribit `ticker.*` notifications.
//
// Uses simdjson On-Demand: no DOM is built, the parser walks the bytes and
// materialises only the fields we ask for. Anything that isn't a ticker
// notification (RPC responses, heartbeats) returns false so the caller can
// fall back to a general JSON parser; those messages are rare.

#include <memory>
#include <string>
#include <string_view>

#include "od/market_data.hpp"

namespace od::deribit {

struct TickerFields {
    std::string_view instrument_name;  // valid until the next parse() call
    TickerUpdate update;               // `instrument`, `recv_ns`, `parsed_ns` left for the caller
};

class TickerParser {
public:
    TickerParser();
    ~TickerParser();
    TickerParser(const TickerParser&) = delete;
    TickerParser& operator=(const TickerParser&) = delete;

    // True if `frame` is a ticker subscription notification and was parsed into `out`.
    bool parse(std::string_view frame, TickerFields& out);

private:
    struct Impl;  // keeps simdjson out of this header
    std::unique_ptr<Impl> impl_;
};

}  // namespace od::deribit
