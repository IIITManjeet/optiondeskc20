#pragma once
// Exchange-agnostic option chain: quotes grouped by expiry.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "od/black76.hpp"

namespace od {

// Deribit instrument names look like BTC-27NOV26-88000-C. Linear (USDC) options
// encode decimal strikes with 'd' (XRP_USDC-27NOV26-0d625-C).
struct InstrumentId {
    std::string currency;
    int year = 0, month = 0, day = 0;
    double strike = 0.0;
    OptionType type = OptionType::Call;
};

std::optional<InstrumentId> parse_option_name(std::string_view name);

struct OptionQuote {
    std::string name;
    OptionType type = OptionType::Call;
    double strike = 0.0;
    std::int64_t expiry_ms = 0;
    // Prices in the coin (inverse contract). 0 means no quote on that side.
    double bid = 0.0, ask = 0.0, mark = 0.0;
    double mark_iv = 0.0;     // exchange's mark IV, decimal (0.3747 = 37.47%)
    double underlying = 0.0;  // forward the exchange marks this option off
    double open_interest = 0.0;
};

struct ExpirySlice {
    std::int64_t expiry_ms = 0;
    std::string label;  // e.g. 27NOV26
    double T = 0.0;     // years from as-of, 365-day basis
    double forward = 0.0;
    std::vector<OptionQuote> quotes;  // sorted by strike, calls before puts
};

struct OptionChain {
    std::string currency;
    std::int64_t asof_ms = 0;
    std::vector<ExpirySlice> expiries;  // sorted by expiry
};

constexpr double kMsPerYear = 365.0 * 24.0 * 3600.0 * 1000.0;

// Groups quotes by expiry, computes T and a per-expiry forward (median of the
// exchange-reported underlying prices).
OptionChain build_chain(std::string currency, std::int64_t asof_ms,
                        std::vector<OptionQuote> quotes);

}  // namespace od
