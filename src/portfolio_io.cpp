#include "od/portfolio_io.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace od {

Portfolio load_portfolio(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    const auto j = nlohmann::json::parse(in);

    Portfolio pf;
    pf.currency = j.value("currency", std::string{"BTC"});
    pf.coin_balance = j.value("coin_balance", 0.0);
    for (const auto& p : j.at("positions")) {
        Position pos;
        pos.instrument = p.at("instrument").get<std::string>();
        pos.kind = pos.instrument.ends_with("PERPETUAL") ? PositionKind::Perpetual
                                                         : PositionKind::Option;
        pos.qty = p.at("qty").get<double>();
        pos.entry_price = p.at("entry_price").get<double>();
        if (pos.kind == PositionKind::Perpetual && pos.entry_price <= 0.0)
            throw std::runtime_error(pos.instrument + ": perpetual needs a positive entry_price");
        pf.positions.push_back(std::move(pos));
    }
    return pf;
}

}  // namespace od
