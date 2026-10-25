#pragma once
// The web UI's snapshot message (`type: "snapshot"`), shared by od_web (live) and
// the demo exporter, so both emit exactly the same shape. The contract is
// documented in docs/UI.md and mirrored by ui/src/types.ts.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "od/chain.hpp"
#include "od/deribit_feed.hpp"
#include "od/surface.hpp"

namespace od::web {

struct FeedInfo {
    std::string bus;
    deribit::FeedStats stats;
    std::size_t live = 0, n_instruments = 0;
    double updates_per_s = 0, trades_per_s = 0;
    double hop_p50_us = 0, hop_p99_us = 0;
    std::uint64_t laps = 0;
    bool stale = false;
};

struct Trade {
    std::string name;
    std::int64_t ts_ms = 0;
    bool buy = false;
    double price = 0, amount = 0, iv = 0, index = 0;
};

// Everything one snapshot is built from.
struct SnapshotInput {
    std::optional<OptionChain> chain;  // nullopt: no bus attached yet
    FeedInfo feed;
    std::vector<Trade> trades;  // newest first
};

// `fits` must come from build_surface(*in.chain). `server_ms` is the wall time the
// snapshot was made: now for live, the recorded time for an export.
nlohmann::json snapshot_json(const SnapshotInput& in, const std::vector<SmileFit>& fits, double fit_ms,
                             std::int64_t server_ms);

}  // namespace od::web
