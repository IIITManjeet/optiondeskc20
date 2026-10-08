#pragma once
// Deribit public REST client (snapshot only; the streaming WebSocket feed is milestone 2).
//
// A Snapshot is the raw exchange data needed to rebuild a chain: the instrument
// list (strike, expiry) and the book summary (bid/ask/mark/IV per instrument).
// Snapshots can be saved and reloaded, so every analysis is reproducible offline.

#include <string>

#include <nlohmann/json.hpp>

#include "od/chain.hpp"
#include "od/market_data.hpp"

namespace od::deribit {

struct Snapshot {
    std::string currency;
    nlohmann::json instruments;  // array, trimmed to the fields we use
    nlohmann::json summary;      // array of book summaries
};

// Throws std::runtime_error on network or API errors.
nlohmann::json public_get(const std::string& method_and_query, bool testnet = false);

Snapshot fetch_snapshot(const std::string& currency, bool testnet = false);
Snapshot load_snapshot(const std::string& path);
void save_snapshot(const Snapshot& snap, const std::string& path);

OptionChain to_chain(const Snapshot& snap);

// Active options for `currency`, as a dense-id table for the streaming feed.
InstrumentTable fetch_instrument_table(const std::string& currency, bool testnet = false);

}  // namespace od::deribit
