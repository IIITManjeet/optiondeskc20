#include "od/chain.hpp"

#include <algorithm>
#include <charconv>
#include <map>

namespace od {

namespace {

int month_from_code(std::string_view m) {
    static constexpr std::string_view kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    for (int i = 0; i < 12; ++i)
        if (m == kMonths[i]) return i + 1;
    return 0;
}

bool to_int(std::string_view s, int& out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

}  // namespace

std::optional<InstrumentId> parse_option_name(std::string_view name) {
    std::string_view parts[4];
    for (int i = 0; i < 4; ++i) {
        const auto dash = name.find('-');
        if (i < 3 && dash == std::string_view::npos) return std::nullopt;
        parts[i] = i < 3 ? name.substr(0, dash) : name;
        if (i < 3) name.remove_prefix(dash + 1);
    }
    if (parts[3].find('-') != std::string_view::npos) return std::nullopt;

    InstrumentId id;
    id.currency = std::string(parts[0]);

    // Expiry: D or DD, then MMM, then YY.
    const auto date = parts[1];
    if (date.size() < 6 || date.size() > 7) return std::nullopt;
    const std::size_t dlen = date.size() - 5;
    int yy = 0;
    if (!to_int(date.substr(0, dlen), id.day) || !to_int(date.substr(dlen + 3), yy))
        return std::nullopt;
    id.month = month_from_code(date.substr(dlen, 3));
    if (id.month == 0 || id.day < 1 || id.day > 31) return std::nullopt;
    id.year = 2000 + yy;

    std::string strike(parts[2]);
    std::replace(strike.begin(), strike.end(), 'd', '.');
    auto [p, ec] = std::from_chars(strike.data(), strike.data() + strike.size(), id.strike);
    if (ec != std::errc{} || p != strike.data() + strike.size() || id.strike <= 0.0)
        return std::nullopt;

    if (parts[3] == "C") id.type = OptionType::Call;
    else if (parts[3] == "P") id.type = OptionType::Put;
    else return std::nullopt;
    return id;
}

OptionChain build_chain(std::string currency, std::int64_t asof_ms,
                        std::vector<OptionQuote> quotes) {
    std::map<std::int64_t, ExpirySlice> by_expiry;
    for (auto& q : quotes) {
        if (q.expiry_ms <= asof_ms) continue;
        auto& slice = by_expiry[q.expiry_ms];
        if (slice.quotes.empty()) {
            slice.expiry_ms = q.expiry_ms;
            const auto dash1 = q.name.find('-');
            const auto dash2 = q.name.find('-', dash1 + 1);
            slice.label = q.name.substr(dash1 + 1, dash2 - dash1 - 1);
            slice.T = static_cast<double>(q.expiry_ms - asof_ms) / kMsPerYear;
        }
        slice.quotes.push_back(std::move(q));
    }

    OptionChain chain{std::move(currency), asof_ms, {}};
    for (auto& [_, slice] : by_expiry) {
        std::vector<double> fwd;
        for (const auto& q : slice.quotes)
            if (q.underlying > 0.0) fwd.push_back(q.underlying);
        if (fwd.empty()) continue;
        std::nth_element(fwd.begin(), fwd.begin() + fwd.size() / 2, fwd.end());
        slice.forward = fwd[fwd.size() / 2];

        std::sort(slice.quotes.begin(), slice.quotes.end(), [](const auto& a, const auto& b) {
            return a.strike != b.strike ? a.strike < b.strike : a.type < b.type;
        });
        chain.expiries.push_back(std::move(slice));
    }
    return chain;
}

}  // namespace od
