#include "od/market_data.hpp"

#include <algorithm>

namespace od {

std::uint32_t InstrumentTable::add(Instrument inst) {
    const auto id = static_cast<std::uint32_t>(list_.size());
    ids_.emplace(inst.name, id);
    list_.push_back(std::move(inst));
    return id;
}

std::optional<std::uint32_t> InstrumentTable::find(std::string_view name) const {
    const auto it = ids_.find(name);
    if (it == ids_.end()) return std::nullopt;
    return it->second;
}

LiveBook::LiveBook(const InstrumentTable& table) : seen_(table.size(), false) {
    quotes_.reserve(table.size());
    for (const auto& inst : table.all()) {
        OptionQuote q;
        q.name = inst.name;
        q.type = inst.type;
        q.strike = inst.strike;
        q.expiry_ms = inst.expiry_ms;
        quotes_.push_back(std::move(q));
    }
}

void LiveBook::apply(const TickerUpdate& u) {
    auto& q = quotes_[u.instrument];
    q.bid = u.bid;
    q.ask = u.ask;
    q.mark = u.mark;
    q.mark_iv = u.mark_iv;
    q.underlying = u.underlying;
    if (!seen_[u.instrument]) seen_[u.instrument] = true, ++live_;
    last_ts_ = std::max(last_ts_, u.exch_ts_ms);
}

OptionChain LiveBook::to_chain(const std::string& currency) const {
    std::vector<OptionQuote> live;
    live.reserve(live_);
    for (std::size_t i = 0; i < quotes_.size(); ++i)
        if (seen_[i]) live.push_back(quotes_[i]);
    return build_chain(currency, last_ts_, std::move(live));
}

}  // namespace od
